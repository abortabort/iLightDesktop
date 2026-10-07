#include "protocol.h"
#include "tables.h"
#include <QtEndian>
#include <QRandomGenerator>
#include <stdexcept>

namespace ilight {
int u(char c) { return static_cast<unsigned char>(c); }
QByteArray command(int type, int cmd, int sub, std::initializer_list<int> args) {
    if (args.size() > 249) throw std::invalid_argument("Command too long");
    QByteArray out; out.append(char(13)); out.append(char(6 + args.size()));
    auto byte = [&out](int n) { if (n < 0 || n > 255) throw std::invalid_argument("Not a byte"); out.append(char(n)); };
    byte(type); byte(cmd); byte(sub); for (int v : args) byte(v); out.append(char(14)); return out;
}
QByteArray frame(const QByteArray &payload, quint16 key, qint32 arg1, qint32 arg2) {
    if (payload.size() > 599) throw std::invalid_argument("Segmentation required");
    int size = (16 + payload.size() + 3) & ~3;
    QByteArray out(size, 0); out[0] = 1; out[1] = char(-2);
    qToBigEndian(key, reinterpret_cast<uchar *>(out.data() + 4));
    qToLittleEndian(quint16(size), reinterpret_cast<uchar *>(out.data() + 6));
    qToLittleEndian(arg1, reinterpret_cast<uchar *>(out.data() + 8));
    qToLittleEndian(arg2, reinterpret_cast<uchar *>(out.data() + 12));
    std::copy(payload.begin(), payload.end(), out.begin() + 16); return out;
}
std::optional<QByteArray> inner(const QByteArray &padded) {
    if (padded.size() < 6) return {};
    int length;
    if (padded[0] == char(13)) {
        length = u(padded[1]);
        if (length < 6 || length > padded.size() || padded[length-1] != char(14)) return {};
    } else if (padded[0] == 'L' && padded[1] == 'X') {
        length = u(padded[2]); if (length != 15 || length > padded.size()) return {};
    } else return {};
    return padded.left(length);
}
void Decoder::feed(const QByteArray &chunk, const std::function<void(const Frame &)> &receiver) {
    // Process incrementally so hostile input cannot grow the retained m_buffer.
    for (char c : chunk) {
        if (m_buffer.size() >= 4096) m_buffer.remove(0, 1);
        m_buffer.append(c);
        while (m_buffer.size() >= 2) {
            if (m_buffer[0] != char(1) || u(m_buffer[1]) != 254) { m_buffer.remove(0,1); continue; }
            if (m_buffer.size() < 8) break;
            int size = qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(m_buffer.constData()+6));
            if (size < 16 || size > 4096 || (size & 3)) { m_buffer.remove(0,1); continue; }
            if (m_buffer.size() < size) break;
            const uchar *data = reinterpret_cast<const uchar *>(m_buffer.constData());
            Frame f { qFromBigEndian<quint16>(data+4), quint8(u(m_buffer[2])),
                qFromLittleEndian<qint32>(data+8), qFromLittleEndian<qint32>(data+12), m_buffer.mid(16, size-16) };
            m_buffer.remove(0,size); receiver(f);
        }
    }
}
static QByteArray swap(const QByteArray &plain) {
    QByteArray out = plain;
    for (int i = 0; i+1 < plain.size(); i += 2) {
        out[i] = char((u(plain[i]) & 7) | (u(plain[i+1]) & 248));
        out[i+1] = char((u(plain[i+1]) & 7) | (u(plain[i]) & 248));
    } return out;
}
Handshake::Handshake(int t, bool enc) : Handshake(t, enc, QRandomGenerator::global()->bounded(10),
    QRandomGenerator::global()->bounded(255), QRandomGenerator::global()->bounded(255), QRandomGenerator::global()->bounded(255)) {}
Handshake::Handshake(int t, bool enc, int digit, int r1, int r2, int r3)
    : m_type(t), m_a(r1), m_b(r2), m_c(r3), m_encrypted(enc) {
    const int temp = int(digit / 10.0 * 64.0); m_key = 64 + temp; m_expected = temp ^ (m_key >> 1);
}
QByteArray Handshake::challenge() const {
    auto plain = command(m_type, 1, 1, {m_key}); return m_encrypted ? encode(plain, m_type, m_a, m_b, m_c) : plain;
}
bool Handshake::accepts(const QByteArray &packet) const {
    auto plain = m_encrypted ? decode(packet,m_a,m_b) : std::optional<QByteArray>(packet);
    return plain && plain->size() == 7 && (*plain)[0] == char(13) && (*plain)[1] == char(7) && (*plain)[3] == char(1)
        && (*plain)[4] == char(1) && u((*plain)[5]) == m_expected && (*plain)[6] == char(14);
}
QByteArray Handshake::encode(const QByteArray &plain, int type, int a, int b, int c) {
    if (plain.size() > 247) throw std::invalid_argument("Cipher length");
    QByteArray out(plain.size()+8,0); out[0]='W'; out[1]='A'; out[2]=char(out.size()); out[3]=char(type);
    out[4]=char(a); out[5]=char(b); out[6]=char(c);
    QByteArray mixed=swap(plain); int sum=0;
    for(int i=0;i<mixed.size();++i) { out[i+8]=char(u(mixed[i]) ^ ((WA[(type+i*b)&255]+a)&255)); sum+=u(out[i+8]); }
    out[7]=char(sum ^ (((a+b)&254)>>1)); return out;
}
std::optional<QByteArray> Handshake::decode(const QByteArray &packet, int a, int b) {
    if(packet.size()!=15 || packet[0]!='L' || packet[1]!='X') return {};
    int sum=0; for(int i=8;i<15;++i) sum+=u(packet[i]);
    if(u(packet[7])!=((sum ^ (((a+b)&254)>>1))&255)) return {};
    QByteArray mixed=packet.mid(8);
    for(int i=0;i<mixed.size();++i) mixed[i]=char(u(mixed[i]) ^ ((LX[(u(packet[3])+i*b)&255]+a)&255));
    return swap(mixed);
}
std::optional<Feedback> parseFeedback(const QByteArray &c) {
    auto valid = inner(c); if (!valid || *valid != c || c[0] != char(13)) return {};
    Feedback f; f.type = u(c[2]); int cmd=u(c[3]), sub=u(c[4]);
    if (cmd!=2 && cmd!=4) return {};
    if (f.type==1 && ((cmd==2 && sub==2) || (cmd==4 && sub==1)) && c.size()==8) {
        if ((c[5] != char(1) && c[5] != char(2)) || u(c[6])<1 || u(c[6])>16) return {};
        f.power=c[5] == char(1); f.brightness=u(c[6]);
    } else if (f.type==1 && sub==3 && c.size()==7) f.warm=u(c[5]);
    else if (f.type==1 && cmd==2 && sub==4 && c.size()==8 && c[5] == char(1) && (c[6] == char(1) || c[6] == char(2))) f.coldWarmSupported=c[6] == char(1);
    else if (f.type==2 && ((cmd==2 && sub==2) || (cmd==4 && sub==1)) && c.size()==12) {
        if (c[5] != char(1) && c[5] != char(2)) return {};
        f.power=c[5] == char(1); f.red=u(c[7]); f.green=u(c[8]); f.blue=u(c[9]); f.effect=u(c[10]);
        // Original ColorLampManager maps byte 6 to brightness and byte 10 to effect.
        f.brightness=u(c[6]);
    } else if (f.type==2 && cmd==2 && sub==13 && c.size()==7) f.colorBrightnessSupported=c[5]==char(1);
    else return {};
    return f;
}
}
