// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "citron/nextendo_online_counts.h"

#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include <fmt/format.h>
#include <QObject>
#include <QTimer>
#include <QWidget>

#ifdef ENABLE_WEB_SERVICE
#include "web_service/nextendo_api.h"
#endif

namespace Nextendo::OnlineCounts {

namespace {

constexpr auto PollInterval = std::chrono::seconds(30);
constexpr auto MaxAge = std::chrono::seconds(60);

std::mutex g_mutex;
std::map<std::string, int> g_counts; // lowercase-hex title id -> player count
std::chrono::steady_clock::time_point g_counts_at;
std::set<u64> g_installed;
bool g_in_flight = false;

class Poller : public QObject {
public:
    using QObject::QObject;

    void Poll(bool forced) {
        if (!forced) {
            const auto* window = qobject_cast<const QWidget*>(parent());
            if (window && (!window->isVisible() || window->isMinimized())) {
                return;
            }
        }
#ifdef ENABLE_WEB_SERVICE
        {
            std::scoped_lock lock{g_mutex};
            if (g_in_flight || (!forced && g_installed.empty())) {
                return;
            }
            g_in_flight = true;
        }
        std::thread{[] {
            auto counts = WebService::NextendoApi::GetOnlineCounts();
            std::scoped_lock lock{g_mutex};
            g_in_flight = false;
            if (!counts.empty()) {
                g_counts = std::move(counts);
                g_counts_at = std::chrono::steady_clock::now();
            }
        }}.detach();
#endif
    }
};

Poller*& PollerInstance() {
    static Poller* instance = nullptr;
    return instance;
}

} // Anonymous namespace

void Start(QObject* parent) {
    Poller*& instance = PollerInstance();
    if (instance) {
        return;
    }

    instance = new Poller(parent);
    auto* timer = new QTimer(instance);
    timer->setInterval(std::chrono::duration_cast<std::chrono::milliseconds>(PollInterval));
    QObject::connect(timer, &QTimer::timeout, instance, [] { PollerInstance()->Poll(false); });
    timer->start();
}

void NoteInstalled(u64 program_id) {
    bool first = false;
    {
        std::scoped_lock lock{g_mutex};
        first = g_installed.empty();
        g_installed.insert(program_id);
    }
    // The first installed title starts counting without waiting for the next tick. The game list
    // reports from its worker thread, so the poll is queued to the poller's thread.
    if (Poller* instance = PollerInstance(); first && instance) {
        QMetaObject::invokeMethod(instance, [instance] { instance->Poll(false); },
                                  Qt::QueuedConnection);
    }
}

void RefreshNow() {
    if (Poller* instance = PollerInstance()) {
        instance->Poll(true);
    }
}

int For(u64 program_id) {
    const std::string key = fmt::format("{:016x}", program_id);
    std::scoped_lock lock{g_mutex};
    if (std::chrono::steady_clock::now() - g_counts_at > MaxAge) {
        return 0;
    }
    const auto it = g_counts.find(key);
    return it != g_counts.end() ? it->second : 0;
}

} // namespace Nextendo::OnlineCounts
