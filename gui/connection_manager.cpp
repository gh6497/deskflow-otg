// SPDX-License-Identifier: Apache-2.0
#include "connection_manager.h"
#include "device_info.h"

ConnectionManager::ConnectionManager(QObject *parent) : QObject(parent)
{
    connect(&m_server, &DeskflowServer::ready, this, &ConnectionManager::launchPending);
    connect(&m_server, &DeskflowServer::changed, this, [this] { emit changed({}); });
    connect(&m_server, &DeskflowServer::log, this, [this](const QString &message) { emit log({}, message); });
    connect(&m_server, &DeskflowServer::failed, this, [this](const QString &id, const QString &detail) {
        auto message = qtTrId(id.toLatin1().constData());
        if (message.contains("%1")) message = message.arg(detail);
        emit log({}, message);
        for (auto it = m_devices.begin(); it != m_devices.end(); ++it) {
            if (state(it.key()) != Session::State::Idle) { it->errorId = id; it->errorDetail = detail; }
        }
        stopAll();
    });
    connect(&m_server, &DeskflowServer::stopped, this, [this] { emit changed({}); });
}

Session *ConnectionManager::addDevice(const QString &id)
{
    if (m_devices.contains(id)) return session(id);
    auto *child = new Session(this);
    m_devices.insert(id, {child, {}, false, {}, {}});
    connect(child, &Session::changed, this, [this, id] { update(id); });
    connect(child, &Session::bleProviderChanged, this, [this, id] { update(id); });
    connect(child, &Session::log, this, [this, id](const QString &message) { emit log(id, message); });
    connect(child, &Session::usbAttached, this, &ConnectionManager::usbAttached);
    return child;
}

ConnectionManager::~ConnectionManager()
{
    // QObject deletes children after members. Disconnect first so a QProcess
    // destructor cannot call into an already destroyed device map.
    m_server.disconnect(this);
    for (auto &entry : m_devices) {
        entry.session->disconnect(this);
        delete entry.session;
    }
    m_devices.clear();
}

Session *ConnectionManager::session(const QString &id) const { return m_devices.value(id).session; }

bool ConnectionManager::removeDevice(const QString &id)
{
    if (!m_devices.contains(id) || busy() || m_server.state() != DeskflowServer::State::Idle || session(id)->hasBleProvider()) return false;
    auto *child = m_devices.take(id).session;
    child->disconnect(this);
    delete child;
    return true;
}

bool ConnectionManager::hasFailure(const QString &id) const
{
    const auto entry = m_devices.value(id);
    return !entry.errorId.isEmpty() || (entry.session && entry.session->hasFailure());
}

Session::State ConnectionManager::state(const QString &id) const
{
    const auto entry = m_devices.value(id);
    return entry.pending ? Session::State::Starting : entry.session ? entry.session->state() : Session::State::Idle;
}

QString ConnectionManager::status(const QString &id) const
{
    const auto entry = m_devices.value(id);
    if (!entry.errorId.isEmpty()) {
        const auto message = qtTrId(entry.errorId.toLatin1().constData());
        return message.contains("%1") ? message.arg(entry.errorDetail) : message;
    }
    if (entry.pending) return qtTrId("otg.status.connecting");
    const auto text = entry.session ? entry.session->statusMessage() : QString();
    return text.isEmpty() ? qtTrId("otg.status.not_connected") : text;
}

bool ConnectionManager::busy() const
{
    for (auto it = m_devices.cbegin(); it != m_devices.cend(); ++it)
        if (state(it.key()) != Session::State::Idle) return true;
    return false;
}

bool ConnectionManager::hasBleProvider() const
{
    for (const auto &entry : m_devices) if (entry.session->hasBleProvider()) return true;
    return false;
}

bool ConnectionManager::settled() const
{
    return !busy() && !hasBleProvider() && m_server.state() == DeskflowServer::State::Idle;
}

bool ConnectionManager::reject(const QString &id, const char *message)
{
    m_devices[id].errorId = QString::fromLatin1(message);
    m_devices[id].errorDetail.clear();
    emit log(id, status(id));
    emit changed(id);
    return false;
}

bool ConnectionManager::start(const QString &id, const ConnectionOptions &options, const QString &layout)
{
    if (m_shutdown || !m_devices.contains(id) || state(id) != Session::State::Idle) return false;
    if (!validScreenName(options.phone) || options.phone.compare(options.computer, Qt::CaseInsensitive) == 0)
        return reject(id, "otg.error.screen_names");
    if (options.manageServer && layout.isEmpty()) return reject(id, "otg.multi.invalid_layout");
    for (auto it = m_devices.cbegin(); it != m_devices.cend(); ++it) {
        if (it.key() == id) continue;
        const bool reserved = state(it.key()) != Session::State::Idle;
        if (reserved && (options.phone.compare(it->options.phone, Qt::CaseInsensitive) == 0 ||
            (options.transport == "usb" && it->options.transport == "usb" && options.serial == it->options.serial)))
            return reject(id, "otg.multi.duplicate_device");
        // Keep one BLE target, including a Windows provider parked while idle.
        if (options.transport == "ble" && ((reserved && it->options.transport == "ble") ||
            it->session->hasBleProvider())) return reject(id, "otg.multi.ble_limit");
    }
    if (busy() || m_server.state() != DeskflowServer::State::Idle) {
        if (options.manageServer != m_shared.manageServer || options.port != m_shared.port ||
            options.host != m_shared.host || options.computer != m_shared.computer ||
            options.deskflow != m_shared.deskflow || (options.manageServer && layout != m_layout))
            return reject(id, "otg.multi.shared_locked");
        if (m_server.state() == DeskflowServer::State::Stopping)
            return reject(id, "otg.multi.shared_locked");
    } else {
        m_shared = options;
        m_layout = layout;
    }
    auto &entry = m_devices[id];
    entry.options = options;
    entry.errorId.clear();
    entry.errorDetail.clear();
    entry.pending = true;
    emit changed(id);
    if (!options.manageServer || m_server.state() == DeskflowServer::State::Running) launchPending();
    else if (m_server.state() == DeskflowServer::State::Idle)
        m_server.start({options.deskflow, options.computer, layout, options.port});
    return true;
}

void ConnectionManager::launchPending()
{
    for (auto it = m_devices.begin(); it != m_devices.end(); ++it) {
        if (!it->pending) continue;
        auto options = it->options;
        options.host = options.manageServer ? "127.0.0.1" : options.host;
        options.manageServer = false; // The manager, never a child session, owns the shared server.
        it->pending = false;
        it->session->start(options);
    }
}

void ConnectionManager::update(const QString &id)
{
    if (!busy()) m_server.stop();
    emit changed(id);
}

void ConnectionManager::stop(const QString &id)
{
    if (!m_devices.contains(id)) return;
    auto &entry = m_devices[id];
    entry.pending = false;
    entry.session->stop();
    update(id);
}

void ConnectionManager::stopAll()
{
    for (auto it = m_devices.begin(); it != m_devices.end(); ++it) it->pending = false;
    for (auto it = m_devices.begin(); it != m_devices.end(); ++it) {
        it->session->stop();
        emit changed(it.key());
    }
    if (!busy()) m_server.stop();
}

void ConnectionManager::shutdown()
{
    m_shutdown = true;
    for (auto &entry : m_devices) entry.pending = false;
    for (auto &entry : m_devices) entry.session->shutdown();
    if (!busy()) m_server.stop();
    emit changed({});
}
