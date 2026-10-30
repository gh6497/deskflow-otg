// SPDX-License-Identifier: Apache-2.0
#include "device_info.h"

#include <QRegularExpression>
#include <QMap>
#include <QSet>
#include <QPoint>

QList<AndroidDevice> parseDevices(const QString &text)
{
    QList<AndroidDevice> result;
    for (const QString &line : text.split('\n')) {
        const auto fields = line.simplified().split(' ');
        if (fields.size() < 2 || line.startsWith("List ") || line.startsWith('*'))
            continue;
        const QString serial = fields[0];
        // AOA requires a physical USB transport, never an emulator or TCP ADB.
        if (serial.startsWith("emulator-") || serial.contains(':') ||
            serial.contains("._adb-tls-"))
            continue;
        if (fields[1] != "device" && fields[1] != "offline" &&
            fields[1] != "unauthorized" && fields[1] != "no")
            continue;
        AndroidDevice device{serial, {}, fields[1]};
        for (const QString &field : fields) {
            if (field.startsWith("model:"))
                device.model = field.mid(6).replace('_', ' ');
        }
        result.append(device);
    }
    return result;
}

QSize parseScreenSize(const QString &text)
{
    const QRegularExpression re("(?:Physical|Override) size:\\s*(\\d+)x(\\d+)");
    auto matches = re.globalMatch(text);
    QSize physical, overrideSize;
    while (matches.hasNext()) {
        const auto match = matches.next();
        const int w = match.captured(1).toInt(), h = match.captured(2).toInt();
        if (w < 1 || h < 1 || w > 32767 || h > 32767)
            continue;
        if (match.captured().startsWith("Override"))
            overrideSize = QSize(w, h);
        else
            physical = QSize(w, h);
    }
    return overrideSize.isValid() ? overrideSize : physical;
}

int parseRotation(const QString &text)
{
    const QRegularExpression re("SurfaceOrientation:\\s*([0-3])\\b");
    auto match = re.match(text);
    if (match.hasMatch())
        return match.captured(1).toInt();
    // Newer InputReader dumps report display viewports instead of the legacy
    // SurfaceOrientation field. Use the primary display, not an external one.
    const QRegularExpression viewport("Viewport[^\\n]*?displayId[ =]+0\\b[^\\n]*?orientation[ =]+([0-3])\\b");
    match = viewport.match(text);
    return match.hasMatch() ? match.captured(1).toInt() : -1;
}

bool validScreenName(const QString &name)
{
    return QRegularExpression("^[A-Za-z0-9_][A-Za-z0-9_.-]{0,62}$").match(name).hasMatch();
}

QString serverConfiguration(const QString &computer, const QString &phone,
                            const QString &direction)
{
    return serverConfiguration(computer, QList<ScreenLink>{{phone, computer, direction}});
}

QString serverConfiguration(const QString &computer, const QList<ScreenLink> &screens)
{
    const QStringList directions{"left", "right", "up", "down"};
    if (!validScreenName(computer) || screens.isEmpty())
        return {};
    QStringList names{computer};
    QSet<QString> unique{computer.toLower()};
    for (const auto &screen : screens) {
        if (!validScreenName(screen.name) || unique.contains(screen.name.toLower()))
            return {};
        unique.insert(screen.name.toLower());
        names.append(screen.name);
    }
    QMap<QString, QMap<QString, QString>> links;
    for (const auto &screen : screens) {
        const int index = directions.indexOf(screen.direction);
        if (index < 0 || !names.contains(screen.neighbor) || screen.neighbor == screen.name)
            return {};
        const auto reverse = directions[index ^ 1];
        if (links[screen.neighbor].contains(screen.direction) || links[screen.name].contains(reverse))
            return {};
        links[screen.neighbor][screen.direction] = screen.name;
        links[screen.name][reverse] = screen.neighbor;
    }
    QSet<QString> reached{computer};
    QMap<QString, QPoint> positions{{computer, {0, 0}}};
    QSet<QPoint> occupied{QPoint()};
    QStringList pending{computer};
    while (!pending.isEmpty()) {
        const auto source = pending.takeFirst();
        const auto edges = links.value(source);
        for (auto edge = edges.cbegin(); edge != edges.cend(); ++edge) {
            const auto target = edge.value();
            if (!reached.contains(target)) {
                const auto direction = edge.key();
                const QPoint step = direction == "left" ? QPoint(-1, 0) : direction == "right" ? QPoint(1, 0) :
                    direction == "up" ? QPoint(0, -1) : QPoint(0, 1);
                const auto point = positions.value(source) + step;
                if (occupied.contains(point)) return {};
                positions[target] = point;
                occupied.insert(point);
                reached.insert(target);
                pending.append(target);
            }
        }
    }
    if (reached.size() != names.size())
        return {};
    QString config = "section: screens\n";
    for (const auto &name : names) config += "  " + name + ":\n";
    config += "end\n\nsection: links\n";
    for (const auto &name : names) {
        config += "  " + name + ":\n";
        for (const auto &direction : directions) {
            if (links.value(name).contains(direction))
                config += "    " + direction + " = " + links.value(name).value(direction) + "\n";
        }
    }
    return config + "end\n";
}
