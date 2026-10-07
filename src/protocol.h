#pragma once
#include <QByteArray>
#include <QVector>
#include <QString>
#include <optional>
#include <functional>

namespace ilight {
int u(char value);
QByteArray command(int type, int cmd, int sub, std::initializer_list<int> args = {});
QByteArray frame(const QByteArray &payload, quint16 key = 0x5181, qint32 arg1 = 0, qint32 arg2 = 0);
std::optional<QByteArray> inner(const QByteArray &padded);
struct Frame { quint16 key; quint8 flags; qint32 arg1, arg2; QByteArray payload; };
class Decoder {
public:
    void feed(const QByteArray &chunk, const std::function<void(const Frame &)> &receiver);
    void clear() { m_buffer.clear(); }
private:
    QByteArray m_buffer;
};
class Handshake {
public:
    Handshake(int type, bool encrypted);
    Handshake(int type, bool encrypted, int digit, int a, int b, int c);
    QByteArray challenge() const;
    bool accepts(const QByteArray &packet) const;
    static QByteArray encode(const QByteArray &plain, int type, int a, int b, int c);
    static std::optional<QByteArray> decode(const QByteArray &packet, int a, int b);
private:
    int m_type, m_key, m_expected, m_a, m_b, m_c;
    bool m_encrypted;
};
struct Feedback {
    int type = 0;
    std::optional<bool> power, coldWarmSupported, colorBrightnessSupported;
    std::optional<int> brightness, warm, effect, red, green, blue;
};
std::optional<Feedback> parseFeedback(const QByteArray &packet);
}
