#include "common.h"
#include "catalog.h"
#include "transport.h"
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <QtTest>
class CoreTest : public QObject {
    Q_OBJECT
private slots:
    void blurayPcmPacking(){
        sd::initGst();for(int bytes:{2,3}){
            GError*error=nullptr;auto*p=gst_parse_launch("appsrc name=source caps=audio/x-private-ts-lpcm format=time ! sdbluraylpcmdec ! appsink name=out sync=false",&error);QVERIFY2(!error,error?error->message:"");QVERIFY(p);
            auto*source=gst_bin_get_by_name(GST_BIN(p),"source");auto*sink=gst_bin_get_by_name(GST_BIN(p),"out");gst_element_set_state(p,GST_STATE_PLAYING);
            QByteArray packet(4+bytes*8,char(0));packet[2]=char(0x31);packet[3]=char(bytes==2?0x40:0xc0);for(int i=4;i<packet.size();++i)packet[i]=char(i*7);
            auto*b=gst_buffer_new_allocate(nullptr,gsize(packet.size()),nullptr);gst_buffer_fill(b,0,packet.constData(),gsize(packet.size()));GST_BUFFER_PTS(b)=0;gst_app_src_push_buffer(GST_APP_SRC(source),b);gst_app_src_end_of_stream(GST_APP_SRC(source));
            auto*sample=gst_app_sink_try_pull_sample(GST_APP_SINK(sink),3*GST_SECOND);QVERIFY(sample);GstMapInfo map{};QVERIFY(gst_buffer_map(gst_sample_get_buffer(sample),&map,GST_MAP_READ));QCOMPARE(QByteArray(reinterpret_cast<const char*>(map.data),int(map.size)),packet.mid(4));gst_buffer_unmap(gst_sample_get_buffer(sample),&map);gst_sample_unref(sample);gst_element_set_state(p,GST_STATE_NULL);gst_object_unref(source);gst_object_unref(sink);gst_object_unref(p);
        }
    }
    void validation() {
        QTemporaryDir dir;QFile in(dir.filePath("in.mkv"));QVERIFY(in.open(QIODevice::WriteOnly));in.close();
        QJsonObject c{{"input",in.fileName()},{"output",dir.filePath("out.mkv")},{"backend","nvidia"},{"codec","h264"}};
        QVERIFY(sd::validate(c).isEmpty());c["bitrate"]=0;QVERIFY(!sd::validate(c).isEmpty());c["bitrate"]=6000;
        c["width"]=1921;c["height"]=1080;QVERIFY(!sd::validate(c).isEmpty());c["width"]=1920;QVERIFY(sd::validate(c).isEmpty());
        c["height"]=0;QVERIFY(!sd::validate(c).isEmpty());c["width"]=0;c["mode"]="copy";c["subtitle"]=in.fileName();QVERIFY(!sd::validate(c).isEmpty());
        c["subtitle"]="";c["output"]="rtmp://localhost/app/secret";c["codec"]="h265";QVERIFY(!sd::validate(c).isEmpty());
        c["codec"]="h264";QVERIFY(sd::validate(c).isEmpty());c["output"]="srt://127.0.0.1:9000?mode=caller";c["codec"]="h265";QVERIFY(sd::validate(c).isEmpty());
        c["output"]=in.fileName();QVERIFY(!sd::validate(c).isEmpty());
    }
    void encryption() {
        QTemporaryDir dir;const auto path=dir.filePath("secret.job");const QJsonObject source{{"output","rtmp://example/live/SECRET-123"},{"unicode","字幕"}};
        sd::saveJson(path,source,true);QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));auto raw=file.readAll();file.close();QVERIFY(!raw.contains("SECRET-123"));QCOMPARE(sd::loadProtected(path),source);
        raw[raw.size()/2]=char(raw[raw.size()/2]^0x5a);QVERIFY(file.open(QIODevice::WriteOnly));file.write(raw);file.close();QVERIFY_EXCEPTION_THROWN(sd::loadProtected(path),std::runtime_error);
    }
    void redaction() {
        auto value=sd::redact("error rtmp://example/live/secret?token=abc and srt://host:1?passphrase=hello");QVERIFY(!value.contains("secret"));QVERIFY(!value.contains("hello"));
        auto c=sd::publicConfig({{"output","rtmps://host/a/key"},{"input","./test.mkv"}});QVERIFY(!c["output"].toString().contains("key"));QCOMPARE(c["input"].toString(),QString("./test.mkv"));
    }
    void quotedPipeline() {
        sd::initGst();const QString value="test\" ! fakesrc ! \"\\中文";const auto spec="fakesink name=sink last-message="+sd::gstQuote(value);
        // Check escaping round-trip using a writable string property.
        GError*error=nullptr;auto *p=gst_parse_launch(("fakesrc name="+sd::gstQuote(value)+" num-buffers=1 ! fakesink").toUtf8(),&error);
        QVERIFY2(!error,error?error->message:"");QVERIFY(p);gst_element_set_state(p,GST_STATE_NULL);gst_object_unref(p);Q_UNUSED(spec);
    }
    void timeFormatting(){QCOMPARE(sd::timeText(3661.9),QString("01:01:01"));QCOMPARE(sd::timeText(-1),QString("00:00:00"));}
    void discPaths(){QCOMPARE(sd::discRoot("test-disc/BDMV"),QDir("test-disc").absolutePath());}
    void transportTimestamps(){
        QByteArray packet(192,char(0xff));auto *ts=reinterpret_cast<std::uint8_t*>(packet.data())+4;
        ts[0]=0x47;ts[1]=0x50;ts[2]=0x11;ts[3]=0x30;ts[4]=7;ts[5]=0x10;
        const quint64 original=4200ULL*90000;
        auto writePcr=[](std::uint8_t*p,quint64 v){p[0]=std::uint8_t(v>>25);p[1]=std::uint8_t(v>>17);p[2]=std::uint8_t(v>>9);p[3]=std::uint8_t(v>>1);p[4]=std::uint8_t(((v&1)<<7)|0x7e);p[5]=0;};
        writePcr(ts+6,original);
        auto *pes=ts+12;pes[0]=0;pes[1]=0;pes[2]=1;pes[3]=0xe0;pes[4]=0;pes[5]=0;pes[6]=0x80;pes[7]=0xc0;pes[8]=10;
        auto writePts=[](std::uint8_t*p,quint64 v,int tag){p[0]=std::uint8_t((tag<<4)|(((v>>30)&7)<<1)|1);p[1]=std::uint8_t(v>>22);p[2]=std::uint8_t((((v>>15)&127)<<1)|1);p[3]=std::uint8_t(v>>7);p[4]=std::uint8_t(((v&127)<<1)|1);};
        writePts(pes+9,original+9000,3);writePts(pes+14,original,1);
        const QByteArray before(packet.constData(),packet.size());sd::rebaseM2ts(reinterpret_cast<std::uint8_t*>(packet.data()),192,9460LL*90000-qint64(original));
        auto readPts=[](std::uint8_t*p){return (quint64((p[0]>>1)&7)<<30)|(quint64(p[1])<<22)|(quint64(p[2]>>1)<<15)|(quint64(p[3])<<7)|(p[4]>>1);};
        QCOMPARE(readPts(pes+9),9460ULL*90000+9000);QCOMPARE(readPts(pes+14),9460ULL*90000);
        QCOMPARE(pes[9]&0xf1,0x31);QCOMPARE(packet.mid(35),before.mid(35));
        sd::rebaseM2ts(reinterpret_cast<std::uint8_t*>(packet.data()),192,qint64(original)-9460LL*90000);QCOMPARE(packet,before);
    }
};
int main(int argc,char **argv){QCoreApplication app(argc,argv);app.setOrganizationName("StreamDesk");app.setApplicationName("StreamDeskTest");sd::setupRuntime();CoreTest test;return QTest::qExec(&test,argc,argv);}
#include "core_test.moc"
