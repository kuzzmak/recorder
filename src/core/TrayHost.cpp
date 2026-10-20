#include "TrayHost.h"

#if defined(Q_OS_LINUX)
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusServiceWatcher>
#include <QDBusInterface>
#include <QDBusReply>
#include <QVariant>

namespace {
constexpr auto kWatcherService = "org.kde.StatusNotifierWatcher";
constexpr auto kWatcherPath    = "/StatusNotifierWatcher";
constexpr auto kWatcherIface   = "org.kde.StatusNotifierWatcher";
}
#endif

void TrayHost::setReady(bool ready)
{
    if (m_ready == ready)
        return;
    m_ready = ready;
    emit readyChanged();
}

#if defined(Q_OS_LINUX)

TrayHost::TrayHost(QObject *parent)
    : QObject(parent)
{
    auto bus = QDBusConnection::sessionBus();

    // React to the watcher service appearing/vanishing (e.g. the appindicators
    // extension starting after us, or being toggled off and on).
    auto *watcher = new QDBusServiceWatcher(
        QString::fromLatin1(kWatcherService), bus,
        QDBusServiceWatcher::WatchForRegistration
            | QDBusServiceWatcher::WatchForUnregistration,
        this);
    connect(watcher, &QDBusServiceWatcher::serviceRegistered, this,
            [this] { reevaluate(); });
    connect(watcher, &QDBusServiceWatcher::serviceUnregistered, this,
            [this] { setReady(false); });

    // The watcher may already be up with a host registered later than its name;
    // listen for the host-registered signal so we don't miss that transition.
    bus.connect(QString::fromLatin1(kWatcherService),
                QString::fromLatin1(kWatcherPath),
                QString::fromLatin1(kWatcherIface),
                QStringLiteral("StatusNotifierHostRegistered"), this,
                SLOT(reevaluate()));

    reevaluate();  // seed initial state for the manual-launch (host already up) case
}

void TrayHost::reevaluate()
{
    auto bus = QDBusConnection::sessionBus();
    auto *iface = bus.interface();
    if (!iface || !iface->isServiceRegistered(QString::fromLatin1(kWatcherService))) {
        setReady(false);
        return;
    }

    // Watcher is up; the tray only renders once a *host* is registered. Ask the
    // watcher directly; on any error, fall back to "watcher present is enough"
    // rather than withholding the icon forever.
    QDBusInterface watcherIface(QString::fromLatin1(kWatcherService),
                                QString::fromLatin1(kWatcherPath),
                                QString::fromLatin1(kWatcherIface), bus);
    const QVariant hosted = watcherIface.property("IsStatusNotifierHostRegistered");
    setReady(hosted.isValid() ? hosted.toBool() : true);
}

#else  // non-Linux: tray is available immediately

TrayHost::TrayHost(QObject *parent)
    : QObject(parent)
{
}

void TrayHost::reevaluate() {}

#endif
