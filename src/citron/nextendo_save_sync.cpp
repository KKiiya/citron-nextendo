// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "citron/nextendo_save_sync.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include <QMessageBox>
#include <QObject>
#include <QPushButton>

#ifdef _WIN32
#include <cstdlib>
#include <fstream>
#include <iterator>
#endif

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/nextendo_compatible_titles.h"
#include "common/logging.h"
#include "common/nextendo_account.h"
#include "core/core.h"
#include "core/file_sys/savedata_factory.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/hle/service/filesystem/filesystem.h"

#ifdef ENABLE_WEB_SERVICE
#include "web_service/nextendo_api.h"
#endif

#ifdef CITRON_ENABLE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

namespace Nextendo::SaveSync {

namespace {

#if defined(CITRON_ENABLE_LIBARCHIVE) || defined(_WIN32)

// Only ever called from the real-backend paths below (libarchive, or the Windows PowerShell
// fallback) -- kept inside this guard so an unused-function error doesn't fire on builds with
// neither (e.g. a non-Windows build without libarchive).
bool IsEligible(u64 title_id) {
    return Nextendo::CompatibleTitles::Table().count(title_id) != 0 &&
          Common::NextendoAccount::IsLinked();
}

bool HasLocalContent(const FileSys::VirtualDir& dir) {
    if (!dir) {
        return false;
    }
    if (!dir->GetFiles().empty()) {
        return true;
    }
    for (const auto& sub : dir->GetSubdirectories()) {
        if (sub && HasLocalContent(sub)) {
            return true;
        }
    }
    return false;
}

#endif // CITRON_ENABLE_LIBARCHIVE || _WIN32

#ifdef CITRON_ENABLE_LIBARCHIVE

void AddDirectoryToArchive(struct archive* a, const FileSys::VirtualDir& dir,
                           const std::string& prefix) {
    for (const auto& file : dir->GetFiles()) {
        if (!file) {
            continue;
        }
        const std::vector<u8> bytes = file->ReadAllBytes();

        struct archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname(entry, (prefix + file->GetName()).c_str());
        archive_entry_set_size(entry, static_cast<la_int64_t>(bytes.size()));
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        if (archive_write_header(a, entry) == ARCHIVE_OK && !bytes.empty()) {
            archive_write_data(a, bytes.data(), bytes.size());
        }
        archive_entry_free(entry);
    }
    for (const auto& sub : dir->GetSubdirectories()) {
        if (sub) {
            AddDirectoryToArchive(a, sub, prefix + sub->GetName() + "/");
        }
    }
}

la_ssize_t ArchiveAppendCallback(struct archive*, void* client_data, const void* buff,
                                 size_t length) {
    auto* out = static_cast<std::vector<u8>*>(client_data);
    const auto* bytes = static_cast<const u8*>(buff);
    out->insert(out->end(), bytes, bytes + length);
    return static_cast<la_ssize_t>(length);
}

std::vector<u8> ZipDirectory(const FileSys::VirtualDir& dir) {
    std::vector<u8> out;
    struct archive* a = archive_write_new();
    if (!a) {
        return out;
    }
    archive_write_set_format_zip(a);
    archive_write_open(a, &out, nullptr, ArchiveAppendCallback, nullptr);
    AddDirectoryToArchive(a, dir, "");
    archive_write_close(a);
    archive_write_free(a);
    return out;
}

bool UnzipToDirectory(std::span<const u8> zip_data, const std::filesystem::path& dest) {
    struct archive* a = archive_read_new();
    struct archive* ext = archive_write_disk_new();
    if (!a || !ext) {
        return false;
    }

    archive_read_support_format_zip(a);
    archive_read_support_filter_all(a);
    // [Nextendo] SECURE_NODOTDOT/SYMLINKS : une entree « ../x » ou un lien ne sort jamais du dossier.
    archive_write_disk_set_options(ext, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
                                            ARCHIVE_EXTRACT_SECURE_NODOTDOT |
                                            ARCHIVE_EXTRACT_SECURE_SYMLINKS);
    archive_write_disk_set_standard_lookup(ext);

    if (archive_read_open_memory(a, zip_data.data(), zip_data.size()) != ARCHIVE_OK) {
        archive_read_free(a);
        archive_write_free(ext);
        return false;
    }

    std::filesystem::create_directories(dest);

    struct archive_entry* entry;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const std::filesystem::path entry_path = dest / archive_entry_pathname(entry);
        archive_entry_set_pathname(entry, entry_path.string().c_str());

        if (archive_write_header(ext, entry) != ARCHIVE_OK) {
            continue;
        }
        if (archive_entry_size(entry) > 0) {
            const void* buff;
            size_t size;
            la_int64_t offset;
            while (archive_read_data_block(a, &buff, &size, &offset) == ARCHIVE_OK) {
                if (archive_write_data_block(ext, buff, size, offset) != ARCHIVE_OK) {
                    break;
                }
            }
        }
        archive_write_finish_entry(ext);
    }

    archive_read_free(a);
    archive_write_free(ext);
    return true;
}

#endif // CITRON_ENABLE_LIBARCHIVE

#if !defined(CITRON_ENABLE_LIBARCHIVE) && defined(_WIN32)

// No libarchive available for this target (e.g. the llvm-mingw cross-compiled Windows build has
// no mingw port of it wired up). GetFullPath() on a save directory is always a real path on
// disk, so shell out to PowerShell's Compress-Archive/Expand-Archive against it directly --
// the same fallback idiom GMainWindow::ExtractZipToDirectory already uses for firmware zips.

std::vector<u8> ZipDirectoryPowerShell(const FileSys::VirtualDir& dir, u64 title_id) {
    const std::filesystem::path real_dir = dir->GetFullPath();
    const std::filesystem::path tmp_zip =
        std::filesystem::temp_directory_path() / fmt::format("nextendo_save_{:016x}.zip", title_id);
    std::error_code ec;
    std::filesystem::remove(tmp_zip, ec);

    // [Nextendo] Ile ACNH : Compress-Archive de PowerShell 5.1 ecrit des chemins a barre inverse,
    // que le serveur refuse ; ZipFile de .NET ecrit « Villager0/personal.dat ».
    const std::string net_cmd = std::string(R"(powershell -NoProfile -NonInteractive -Command "Add-Type -AssemblyName System.IO.Compression.FileSystem; [IO.Compression.ZipFile]::CreateFromDirectory(')") +
                                real_dir.string() + R"(', ')" + tmp_zip.string() + R"x(')")x";
    const std::string cmd = title_id == 0x01006F8002326000ULL ? net_cmd : "powershell -NoProfile -NonInteractive -Command \"Compress-Archive -Path \\\"" +
                             real_dir.string() + "\\*\\\" -DestinationPath \\\"" + tmp_zip.string() +
                             "\\\" -Force\"";
    if (std::system(cmd.c_str()) != 0) {
        std::filesystem::remove(tmp_zip, ec);
        return {};
    }

    std::vector<u8> out;
    {
        // Windows can't delete a file with an open handle (unlike POSIX) -- close in first.
        std::ifstream in(tmp_zip, std::ios::binary);
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    std::filesystem::remove(tmp_zip, ec);
    return out;
}

bool UnzipToDirectoryPowerShell(std::span<const u8> zip_data, const std::filesystem::path& dest,
                                 u64 title_id) {
    std::filesystem::create_directories(dest);
    const std::filesystem::path tmp_zip = std::filesystem::temp_directory_path() /
                                           fmt::format("nextendo_save_in_{:016x}.zip", title_id);
    {
        std::ofstream out(tmp_zip, std::ios::binary);
        if (!out) {
            return false;
        }
        out.write(reinterpret_cast<const char*>(zip_data.data()),
                  static_cast<std::streamsize>(zip_data.size()));
    }

    const std::string cmd = "powershell -NoProfile -NonInteractive -Command \"Expand-Archive -Path \\\"" +
                             tmp_zip.string() + "\\\" -DestinationPath \\\"" + dest.string() +
                             "\\\" -Force\"";
    const bool ok = std::system(cmd.c_str()) == 0;
    std::error_code ec;
    std::filesystem::remove(tmp_zip, ec);
    return ok;
}

#endif // !CITRON_ENABLE_LIBARCHIVE && _WIN32

#if defined(CITRON_ENABLE_LIBARCHIVE) || defined(_WIN32)

// ---- [Nextendo] Ile d'Animal Crossing: New Horizons : synchro STRICTE -----------------------
// Une ile par compte cote serveur, sans copie ; une revision ; envoi conditionnel. Meme regle
// que Ryujinx (NextendoIslandSync) : rien n'est remplace sans le choix du joueur.

constexpr u64 AcnhTitleId = 0x01006F8002326000ULL;

bool IsStrict(u64 title_id) {
    return title_id == AcnhTitleId;
}

struct StrictState {
    bool present = false;
    u64 rev = 0;
    std::string hash;
};

std::filesystem::path StatePath(u64 title_id) {
    return Common::FS::GetCitronPath(Common::FS::CitronPath::ConfigDir) / "nextendo_saves" /
           fmt::format("{}_{:016x}.txt", Common::NextendoAccount::GetPid(), title_id);
}

StrictState LoadState(u64 title_id) {
    StrictState st;
    std::ifstream in(StatePath(title_id));
    if (in >> st.rev >> st.hash) {
        st.present = true;
    }
    return st;
}

// Empreinte du contenu (chemins tries, tailles, octets), FNV-1a 64 : sert seulement a savoir si
// l'ile locale a change depuis la derniere synchro.
void CollectFiles(const FileSys::VirtualDir& dir, const std::string& prefix,
                  std::map<std::string, FileSys::VirtualFile>& out) {
    for (const auto& f : dir->GetFiles()) {
        if (f) {
            out.emplace(prefix + f->GetName(), f);
        }
    }
    for (const auto& sub : dir->GetSubdirectories()) {
        if (sub) {
            CollectFiles(sub, prefix + sub->GetName() + "/", out);
        }
    }
}

std::string Fingerprint(const FileSys::VirtualDir& dir) {
    if (!dir || !HasLocalContent(dir)) {
        return "-";
    }
    std::map<std::string, FileSys::VirtualFile> files;
    CollectFiles(dir, "", files);
    u64 h = 0xcbf29ce484222325ULL;
    const auto mix = [&h](const u8* p, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            h = (h ^ p[i]) * 0x100000001b3ULL;
        }
    };
    for (const auto& [name, f] : files) {
        mix(reinterpret_cast<const u8*>(name.data()), name.size());
        const u64 size = f->GetSize();
        mix(reinterpret_cast<const u8*>(&size), sizeof(size));
        const auto bytes = f->ReadAllBytes();
        mix(bytes.data(), bytes.size());
    }
    return fmt::format("{:016x}", h);
}

// Hash de la derniere capture, pour l'envoi qui suit sur un autre fil.
std::mutex g_capture_mutex;
std::string g_capture_hash;

#ifdef ENABLE_WEB_SERVICE

void SaveState(u64 title_id, u64 rev, const std::string& hash) {
    const auto p = StatePath(title_id);
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    const auto tmp = std::filesystem::path(p.string() + ".tmp");
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << rev << ' ' << (hash.empty() ? std::string("-") : hash) << std::endl;
    }
    std::filesystem::rename(tmp, p, ec);
}

std::string DeviceId() {
    static std::mutex m;
    static std::string id;
    std::lock_guard lk{m};
    if (!id.empty()) {
        return id;
    }
    const auto p =
        Common::FS::GetCitronPath(Common::FS::CitronPath::ConfigDir) / "nextendo_device_id.txt";
    std::string hex;
    {
        std::ifstream in(p);
        in >> hex;
    }
    if (hex.size() != 8 || !std::all_of(hex.begin(), hex.end(), [](char c) {
            return std::isxdigit(static_cast<unsigned char>(c)) != 0;
        })) {
        std::random_device rd;
        hex = fmt::format("{:08x}", rd());
        std::ofstream out(p, std::ios::trunc);
        out << hex;
    }
    id = "citron:" + hex;
    return id;
}

enum class LaunchAction { Nothing, OfferFirstUpload, OfferDownload, Conflict };

LaunchAction Decide(bool cloud_exists, u64 cloud_rev, bool local_exists,
                    const std::string& local_hash, const StrictState& st) {
    if (!cloud_exists) {
        return local_exists ? LaunchAction::OfferFirstUpload : LaunchAction::Nothing;
    }
    if (!local_exists) {
        return LaunchAction::OfferDownload;
    }
    if (!st.present) {
        return LaunchAction::Conflict;
    }
    if (st.rev == cloud_rev) {
        return LaunchAction::Nothing;
    }
    return st.hash == local_hash ? LaunchAction::OfferDownload : LaunchAction::Conflict;
}

bool ReplaceWithCloud(const FileSys::VirtualDir& save_dir, u64 title_id) {
    u64 rev = 0;
    const auto zip = WebService::NextendoApi::PullStrictSave(fmt::format("{:016x}", title_id), rev);
    if (!zip || zip->empty() || rev == 0) {
        return false;
    }
    // L'ile du cloud remplace TOUT le dossier : un habitant parti ne doit pas rester en local.
    const std::filesystem::path dest = save_dir->GetFullPath();
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dest, ec)) {
        std::filesystem::remove_all(e.path(), ec);
    }
#ifdef CITRON_ENABLE_LIBARCHIVE
    const bool ok = UnzipToDirectory(*zip, dest);
#else
    const bool ok = UnzipToDirectoryPowerShell(*zip, dest, title_id);
#endif
    if (ok) {
        SaveState(title_id, rev, Fingerprint(save_dir));
        LOG_INFO(Frontend, "Nextendo island {:016X}: rev {} downloaded ({} B)", title_id, rev,
                 zip->size());
    }
    return ok;
}

bool SendStrict(const FileSys::VirtualDir& save_dir, u64 title_id, u64 base_rev) {
#ifdef CITRON_ENABLE_LIBARCHIVE
    const auto zip = ZipDirectory(save_dir);
#else
    const auto zip = ZipDirectoryPowerShell(save_dir, title_id);
#endif
    if (zip.empty()) {
        return false;
    }
    u64 new_rev = 0;
    WebService::NextendoApi::SaveMeta cloud;
    const auto r = WebService::NextendoApi::PushStrictSave(
        fmt::format("{:016x}", title_id), zip, base_rev, DeviceId(), new_rev, cloud);
    if (r == WebService::NextendoApi::StrictPush::Sent) {
        SaveState(title_id, new_rev, Fingerprint(save_dir));
        return true;
    }
    LOG_WARNING(Frontend, "Nextendo island {:016X}: upload {} (cloud rev {})", title_id,
                r == WebService::NextendoApi::StrictPush::Conflict ? "CONFLICT" : "failed",
                cloud.rev);
    return false;
}

void PullStrict(const FileSys::VirtualDir& save_dir, u64 title_id) {
    const std::string tid = fmt::format("{:016x}", title_id);
    const auto meta = WebService::NextendoApi::GetSaveMeta(tid, DeviceId());
    if (!meta.ok) {
        return; // serveur injoignable : l'envoi conditionnel protegera le cloud
    }
    const QString title = QObject::tr("Your island in the cloud");
    if (!meta.session_device.empty() && meta.session_device != DeviceId()) {
        QMessageBox::warning(
            nullptr, title,
            QObject::tr("Your island seems to be open on %1. If you play here at the same time, "
                        "the two games will diverge and one of them will have to be dropped when "
                        "syncing.")
                .arg(QString::fromStdString(meta.session_device)));
    }
    const bool local = HasLocalContent(save_dir);
    const std::string hash = Fingerprint(save_dir);
    const StrictState st = LoadState(title_id);
    const LaunchAction action = Decide(meta.exists, meta.rev, local, hash, st);
    LOG_INFO(Frontend, "Nextendo island {:016X}: cloud rev={} local={} state={} -> {}", title_id,
             meta.rev, local, st.present ? std::to_string(st.rev) : std::string("none"),
             static_cast<int>(action));

    switch (action) {
    case LaunchAction::OfferFirstUpload:
        // Question c : l'ile de cet emulateur se lie au compte SEULEMENT si le joueur le dit.
        if (QMessageBox::question(
                nullptr, title,
                QObject::tr("No island is stored in the cloud for your account. Link this "
                            "emulator's island to your Nextendo account? You will then be able to "
                            "get it back on Ryujinx, Citron or your Switch.")) == QMessageBox::Yes) {
            SendStrict(save_dir, title_id, 0);
        }
        break;
    case LaunchAction::OfferDownload:
        if (QMessageBox::question(
                nullptr, title,
                QObject::tr("Your cloud island (saved on %1 from %2) is newer than this emulator's. "
                            "Download it? This emulator's island will be replaced.")
                    .arg(QString::fromStdString(meta.updated_at),
                         QString::fromStdString(meta.device))) == QMessageBox::Yes) {
            ReplaceWithCloud(save_dir, title_id);
        }
        break;
    case LaunchAction::Conflict: {
        // Question b : deux iles differentes, le joueur choisit ; Annuler ne touche a rien.
        QMessageBox box(QMessageBox::Question, title,
                        QObject::tr("Two different islands: the cloud one (saved on %1 from %2) and "
                                    "this emulator's. Only one island is kept per account: the "
                                    "other will be lost.")
                            .arg(QString::fromStdString(meta.updated_at),
                                 QString::fromStdString(meta.device)));
        QPushButton* keep_cloud =
            box.addButton(QObject::tr("Keep the cloud island"), QMessageBox::AcceptRole);
        QPushButton* keep_local =
            box.addButton(QObject::tr("Keep this emulator's island"), QMessageBox::DestructiveRole);
        box.addButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() == keep_cloud) {
            ReplaceWithCloud(save_dir, title_id);
        } else if (box.clickedButton() == keep_local) {
            SendStrict(save_dir, title_id, meta.rev);
        }
        break;
    }
    case LaunchAction::Nothing:
        break;
    }
    if (LoadState(title_id).present) {
        WebService::NextendoApi::AnnounceSaveSession(tid, DeviceId());
    }
}

#endif // ENABLE_WEB_SERVICE

#endif // CITRON_ENABLE_LIBARCHIVE || _WIN32

} // namespace

void Pull(Core::System& system, u64 title_id, bool force) {
#if defined(ENABLE_WEB_SERVICE) && (defined(CITRON_ENABLE_LIBARCHIVE) || defined(_WIN32))
    if (!IsEligible(title_id)) {
        return;
    }

    auto save_dir = system.GetFileSystemController().GetSaveDataFactory().GetTitleSaveDirectory(
        title_id);
    if (IsStrict(title_id)) {
        if (save_dir) {
            PullStrict(save_dir, title_id);
        }
        return;
    }
    if (!force && HasLocalContent(save_dir)) {
        LOG_INFO(Frontend, "Nextendo save pull {:016X}: local save present -> kept (no overwrite)",
                 title_id);
        return;
    }
    if (!save_dir) {
        return;
    }

    const auto zip = WebService::NextendoApi::PullSave(fmt::format("{:016x}", title_id));
    if (!zip || zip->empty()) {
        return;
    }

#ifdef CITRON_ENABLE_LIBARCHIVE
    const bool applied = UnzipToDirectory(*zip, save_dir->GetFullPath());
#else
    const bool applied = UnzipToDirectoryPowerShell(*zip, save_dir->GetFullPath(), title_id);
#endif
    if (applied) {
        LOG_INFO(Frontend, "Nextendo save pull {:016X}: applied ({} B)", title_id, zip->size());
    }
#else
    (void)system;
    (void)title_id;
#endif
}

std::vector<u8> CaptureForPush(Core::System& system, u64 title_id) {
#if defined(CITRON_ENABLE_LIBARCHIVE) || defined(_WIN32)
    if (!IsEligible(title_id)) {
        return {};
    }
    auto save_dir = system.GetFileSystemController().GetSaveDataFactory().GetTitleSaveDirectory(
        title_id);
    if (!save_dir) {
        return {};
    }
    if (IsStrict(title_id)) {
        // Ile ACNH : rien a envoyer sans lien explicite (etat absent) ni si rien n'a change.
        const StrictState st = LoadState(title_id);
        const std::string hash = Fingerprint(save_dir);
        if (!st.present || hash == st.hash) {
            return {};
        }
        std::lock_guard lk{g_capture_mutex};
        g_capture_hash = hash;
    }
#ifdef CITRON_ENABLE_LIBARCHIVE
    return ZipDirectory(save_dir);
#else
    return ZipDirectoryPowerShell(save_dir, title_id);
#endif
#else
    (void)system;
    (void)title_id;
    return {};
#endif
}

void UploadCaptured(u64 title_id, std::vector<u8> zip_bytes) {
#ifdef ENABLE_WEB_SERVICE
    if (zip_bytes.empty()) {
        return;
    }
#if defined(CITRON_ENABLE_LIBARCHIVE) || defined(_WIN32)
    if (IsStrict(title_id)) {
        // Envoi conditionnel sur la revision de la derniere synchro : en cas de conflit (ile
        // jouee ailleurs entre-temps), rien n'est ecrase et le prochain lancement demande.
        const StrictState st = LoadState(title_id);
        if (!st.present) {
            return;
        }
        std::string hash;
        {
            std::lock_guard lk{g_capture_mutex};
            hash = g_capture_hash;
        }
        u64 new_rev = 0;
        WebService::NextendoApi::SaveMeta cloud;
        const auto r = WebService::NextendoApi::PushStrictSave(
            fmt::format("{:016x}", title_id), zip_bytes, st.rev, DeviceId(), new_rev, cloud);
        if (r == WebService::NextendoApi::StrictPush::Sent) {
            SaveState(title_id, new_rev, hash);
            LOG_INFO(Frontend, "Nextendo island {:016X}: rev {} sent ({} B)", title_id, new_rev,
                     zip_bytes.size());
        } else {
            LOG_WARNING(Frontend, "Nextendo island {:016X}: upload {} (cloud rev {})", title_id,
                        r == WebService::NextendoApi::StrictPush::Conflict ? "CONFLICT" : "failed",
                        cloud.rev);
        }
        return;
    }
#endif
    const std::string error =
        WebService::NextendoApi::PushSave(fmt::format("{:016x}", title_id), zip_bytes);
    if (!error.empty()) {
        LOG_WARNING(Frontend, "Nextendo save push {:016X} failed: {}", title_id, error);
    } else {
        LOG_INFO(Frontend, "Nextendo save push {:016X}: {} B", title_id, zip_bytes.size());
    }
#else
    (void)title_id;
    (void)zip_bytes;
#endif
}

} // namespace Nextendo::SaveSync
