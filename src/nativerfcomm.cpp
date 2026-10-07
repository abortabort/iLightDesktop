#include <winsock2.h>
#include <ws2bth.h>
#include <windows.h>
#include "nativerfcomm.h"
#include "bleadvertisements.h"
#include <QElapsedTimer>
#include <QMutexLocker>

namespace {
QString socketError(int code) {
    wchar_t *message = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
        | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, DWORD(code), 0, reinterpret_cast<wchar_t *>(&message), 0, nullptr);
    const QString text = length ? QString::fromWCharArray(message, int(length)).trimmed() : QStringLiteral("Winsock error");
    if (message) LocalFree(message);
    return QStringLiteral("%1 (WSA %2)").arg(text).arg(code);
}
}

NativeRfcomm::NativeRfcomm(const QString &address) : m_address(address) {}
NativeRfcomm::~NativeRfcomm() { requestStop(); wait(); }
void NativeRfcomm::closeSocket() {
    const SOCKET socket = SOCKET(m_socket.exchange(~quintptr(0)));
    if (socket != INVALID_SOCKET) { ::shutdown(socket, SD_BOTH); ::closesocket(socket); }
}
void NativeRfcomm::requestStop() { m_stopping = true; closeSocket(); }
bool NativeRfcomm::enqueue(const QByteArray &bytes) {
    QMutexLocker locker(&m_queueMutex);
    if (m_stopping || m_socket == ~quintptr(0) || m_queuedBytes + bytes.size() > 16384) return false;
    m_queue.enqueue(bytes); m_queuedBytes += bytes.size(); return true;
}

void NativeRfcomm::run() {
    WSADATA data {};
    const int startup = WSAStartup(MAKEWORD(2, 2), &data);
    if (startup) { emit failed(socketError(startup)); return; }
    auto finish = [this] { closeSocket(); WSACleanup(); };
    SOCKET socket = ::socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
    if (socket == INVALID_SOCKET) {
        const int error = WSAGetLastError(); finish(); emit failed(socketError(error)); return;
    }
    m_socket = quintptr(socket);
    if (m_stopping) { finish(); return; }
    u_long nonblocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        const int error = WSAGetLastError(); finish(); emit failed(socketError(error)); return;
    }
    SOCKADDR_BTH remote {};
    remote.addressFamily = AF_BTH;
    QString digits = m_address; digits.remove(':');
    bool valid = false; remote.btAddr = digits.toULongLong(&valid, 16);
    if (!valid) { finish(); emit failed(QStringLiteral("蓝牙地址无效")); return; }
    remote.serviceClassId = GUID {0x00001101, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb}};
    remote.port = 0; // Resolve SPP through the Windows socket provider, not WinRT device objects.
    emit diagnostic(QStringLiteral("NATIVE RFCOMM connect address=%1 service=SPP auth=default port=auto").arg(m_address));
    int result = ::connect(socket, reinterpret_cast<SOCKADDR *>(&remote), sizeof(remote));
    if (result == SOCKET_ERROR) {
        int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS) {
            finish();
            if (!m_stopping) inspectBleAdvertisements(m_address, m_stopping);
            if (!m_stopping) emit failed(socketError(error)); return;
        }
        QElapsedTimer deadline; deadline.start();
        bool established = false;
        while (!m_stopping && deadline.elapsed() < 12000) {
            fd_set writeSet, errorSet; FD_ZERO(&writeSet); FD_ZERO(&errorSet);
            FD_SET(socket, &writeSet); FD_SET(socket, &errorSet);
            timeval interval {0, 200000};
            result = ::select(0, nullptr, &writeSet, &errorSet, &interval);
            if (result == SOCKET_ERROR) { error = WSAGetLastError(); break; }
            if (result > 0) {
                int size = sizeof(error); error = 0;
                if (getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &size) == SOCKET_ERROR) error = WSAGetLastError();
                established = error == 0 && FD_ISSET(socket, &writeSet); break;
            }
        }
        if (!established) {
            if (!error || error == WSAEWOULDBLOCK || error == WSAEINPROGRESS) error = WSAETIMEDOUT;
            finish();
            if (!m_stopping) inspectBleAdvertisements(m_address, m_stopping);
            if (!m_stopping) emit failed(socketError(error)); return;
        }
    }
    if (m_stopping) { finish(); return; }
    SOCKADDR_BTH peer {}; int peerSize = sizeof(peer);
    const int peerResult = getpeername(socket, reinterpret_cast<SOCKADDR *>(&peer), &peerSize);
    emit diagnostic(QStringLiteral("NATIVE RFCOMM connected channel=%1").arg(peerResult == 0 ? int(peer.port) : -1));
    emit connected();
    QByteArray pending; int offset = 0;
    QString failure;
    while (!m_stopping) {
        if (pending.isEmpty()) {
            QMutexLocker locker(&m_queueMutex);
            if (!m_queue.isEmpty()) { pending = m_queue.dequeue(); m_queuedBytes -= pending.size(); offset = 0; }
        }
        fd_set readSet, writeSet; FD_ZERO(&readSet); FD_ZERO(&writeSet);
        FD_SET(socket, &readSet); if (!pending.isEmpty()) FD_SET(socket, &writeSet);
        timeval interval {0, 100000};
        result = ::select(0, &readSet, pending.isEmpty() ? nullptr : &writeSet, nullptr, &interval);
        if (result == SOCKET_ERROR) { failure = socketError(WSAGetLastError()); break; }
        if (m_stopping) break;
        if (!pending.isEmpty() && FD_ISSET(socket, &writeSet)) {
            const int count = ::send(socket, pending.constData() + offset, pending.size() - offset, 0);
            if (count == SOCKET_ERROR) {
                const int error = WSAGetLastError(); if (error != WSAEWOULDBLOCK) { failure = socketError(error); break; }
            } else if (count <= 0) { failure = QStringLiteral("RFCOMM 写入已中断"); break; }
            else { offset += count; if (offset == pending.size()) pending.clear(); }
        }
        if (FD_ISSET(socket, &readSet)) {
            char buffer[1024]; const int count = ::recv(socket, buffer, sizeof(buffer), 0);
            if (count == 0) { failure = QStringLiteral("灯具关闭了 RFCOMM 连接"); break; }
            if (count == SOCKET_ERROR) {
                const int error = WSAGetLastError(); if (error != WSAEWOULDBLOCK) { failure = socketError(error); break; }
            } else emit received(QByteArray(buffer, count));
        }
    }
    finish();
    if (!m_stopping && !failure.isEmpty()) emit failed(failure);
}
