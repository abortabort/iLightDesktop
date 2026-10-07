#include "session.h"
#include <limits>

LampSession::LampSession(QObject *parent) : QObject(parent) {
    for (auto timer : {&m_timeout, &m_queryFallback, &m_refreshTimer}) timer->setSingleShot(true);
    connect(&m_timeout,&QTimer::timeout,this,[this] {
        if (m_active && !m_ready) { stop(); emit failed(QStringLiteral("灯具状态读取超时，请断开手机连接后重试")); }
    });
    connect(&m_queryFallback,&QTimer::timeout,this,&LampSession::queryVersion);
    connect(&m_refreshTimer,&QTimer::timeout,this,[this] { queryState(m_refreshType); });
}
void LampSession::start(int lampType, const QString &mac) {
    stop(); m_active=true; m_type=lampType; m_address=mac.toUpper();
    emit status(QStringLiteral("已连接，正在读取灯具信息…"),false);
    // BluzManager.e() requests QUE_SUPPORT_FEATURE (0x5102); 0x5100 is global status.
    emit outgoing(ilight::frame({},0x5102,std::numeric_limits<qint32>::min(),std::numeric_limits<qint32>::min()));
    m_queryFallback.start(3000); m_timeout.start(12000);
}
void LampSession::stop() {
    ++m_generation; m_active=false; m_ready=false; m_querying=false; m_handshake.reset(); m_decoder.clear(); m_version.clear();
    m_timeout.stop(); m_queryFallback.stop(); m_refreshTimer.stop();
}
void LampSession::later(int ms,const std::function<void()> &task) {
    int token=m_generation; QTimer::singleShot(ms,this,[this,token,task] { if (m_active && token==m_generation) task(); });
}
void LampSession::sendInner(const QByteArray &command) { if (m_active) emit outgoing(ilight::frame(command)); }
void LampSession::queryVersion() {
    if (!m_active || m_querying) return; m_querying=true; m_queryFallback.stop();
    sendInner(ilight::command(m_type,2,1,{0}));
    later(50,[this] { sendInner(ilight::command(255,2,1,{0})); });
    later(100,[this] { sendInner(ilight::command(255,2,11)); });
    // The Android app initializes the lamp UI after one second, independently
    // of version/challenge replies. Some firmware only answers lamp state queries.
    later(1000,[this] {
        queryState(m_type);
        later(250,[this] { queryState(m_type == 1 ? 2 : 1); });
    });
    later(5000,[this] {
        if (m_ready || m_handshake || m_address.startsWith("C9:5C")) return;
        m_handshake=std::make_unique<ilight::Handshake>(m_type,false);
        sendInner(m_handshake->challenge());
    });
}
void LampSession::receive(const QByteArray &bytes) {
    if (m_active) m_decoder.feed(bytes,[this](const ilight::Frame &f) { handle(f); });
}
void LampSession::handle(const ilight::Frame &frame) {
    if (!m_active) return;
    if (frame.key==0x4102 && !m_querying) {
        emit status(QStringLiteral("已连接，正在初始化灯具功能…"),false);
        if (quint32(frame.arg1) & (1u << 4)) {
            // Original manager also retrieves folders before reporting ready when feature 4 is set.
            emit outgoing(ilight::frame({},0x5103,std::numeric_limits<qint32>::min(),std::numeric_limits<qint32>::min()));
            m_queryFallback.start(1500);
        } else queryVersion();
        return;
    }
    if (frame.key==0x4103 && !m_querying) { queryVersion(); return; }
    if (frame.key!=0x4181 || (frame.flags & 0x1c)) return;
    auto packet=ilight::inner(frame.payload); if (!packet) return;
    const auto &c=*packet;
    if (!m_ready && !m_handshake && c[0]==13 && c[3]==2 && c[4]==1 && (c.size()==9 || c.size()==15)) {
        m_version=QStringLiteral("%1.%2.%3").arg(ilight::u(c[5])).arg(ilight::u(c[6])).arg(ilight::u(c[7]));
        const int responseType = ilight::u(c[2]);
        m_handshake=std::make_unique<ilight::Handshake>(responseType == 1 || responseType == 2 ? responseType : m_type,
            m_address.startsWith("C9:5C") || c.size()==15);
        emit status(QStringLiteral("固件 %1 · 正在验证…").arg(m_version),false);
        sendInner(m_handshake->challenge());
    } else if (!m_ready && m_handshake && m_handshake->accepts(c)) {
        m_ready=true; m_timeout.stop();
        emit status(m_version.isEmpty() ? QStringLiteral("已连接 · 验证通过")
            : QStringLiteral("已连接 · 验证通过 · 固件 %1").arg(m_version),true);
        queryState(m_type);
    } else if (c[0]==13) {
        const auto state=ilight::parseFeedback(c);
        if (!m_ready && state && state->power.has_value()) {
            m_ready=true; m_timeout.stop();
            emit status(QStringLiteral("已连接 · 灯具状态读取成功"),true);
        }
        if (m_ready) emit feedback(c);
    }
}
void LampSession::control(const QByteArray &command,int lampType) {
    if (!isReady()) return;
    sendInner(command); m_refreshType=lampType; m_refreshTimer.start(180);
}
void LampSession::queryState(int lampType) {
    if (!m_active) return;
    if (lampType==1) {
        sendInner(ilight::command(1,2,2,{0,0}));
        later(60,[this] { sendInner(ilight::command(1,2,3,{0})); });
        later(120,[this] { sendInner(ilight::command(1,2,4,{1,0})); });
    } else {
        sendInner(ilight::command(2,2,2,{0,0,0,0,0,0}));
        later(30,[this] { sendInner(ilight::command(2,2,13)); });
    }
}
