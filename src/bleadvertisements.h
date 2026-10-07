#ifndef BLEADVERTISEMENTS_H
#define BLEADVERTISEMENTS_H
#include <QString>
#include <atomic>
void inspectBleAdvertisements(const QString &address, const std::atomic<bool> &stopping);
#endif
