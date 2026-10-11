#pragma once

#include "backend/nvcomputer.h"
#include <QVariantList>
#include <QVariantMap>

// Discovery status is authoritative. Cached apps and pairing state do not
// determine reachability, and unknown hosts have not been confirmed online.
inline QVariantList onlineComputerEntries(const QVector<NvComputer*>& computers)
{
    QVariantList entries;
    for (const auto* computer : computers) {
        QReadLocker lock(&computer->lock);
        if (computer->state == NvComputer::CS_ONLINE) {
            entries.append(QVariantMap{{"name", computer->name}, {"uuid", computer->uuid}});
        }
    }
    return entries;
}
