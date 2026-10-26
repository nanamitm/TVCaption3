#ifndef INCLUDE_TTML_CAPTION_HPP
#define INCLUDE_TTML_CAPTION_HPP

// 4K/8K放送の字幕(ARIB-TTML)をB24に変換されたものではなくTTMLのまま表示するため
// の受信と変換。dantto4kがTSのデータ放送用PID(PMTのregistration_descriptorが'D4KA')
// に載せた字幕(table_id 0xC1)を受け取り、dantto4kのparser/resolverで解析して、
// libaribcaptionのCaptionに変換する。描画はB24の字幕と同じRendererで行う。

#include "aribcaption/caption.hpp"
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace TtmlCaption
{

// dantto4kのデータ放送用ESのformat_identifier
const uint32_t FORMAT_IDENTIFIER_D4KA = 0x44344B41;

// 字幕の文書かリソース
struct Document
{
    // 0x30-0x37が字幕、0x38-0x3Fが文字スーパー
    int componentTag = 0;
    // 0がTTML文書、6がSVGフォント(外字)
    int dataType = 0;
    // 1111は時刻制御なし
    int tmd = 0;
    // 文書の時刻0を提示するPTS(90kHz)。負ならすぐに表示する
    int64_t pts = -1;
    // 0:1920x1080、1:3840x2160、2:7680x4320
    int resolution = 1;
    std::string data;
};

// データ放送用PIDのTSパケットから字幕のセクションを組み立てる
class SectionReceiver
{
public:
    void Reset();
    // 188バイトのTSパケット
    void FeedPacket(const uint8_t *packet);
    std::vector<Document> TakeDocuments();

private:
    void OnSection(const uint8_t *section, size_t size);
    void OnCaption(const std::vector<uint8_t> &body);

    struct PendingSections
    {
        int lastSectionNumber = -1;
        std::map<int, std::vector<uint8_t>> parts;
    };
    std::vector<uint8_t> m_buffer;
    int m_continuityCounter = -1;
    bool m_synced = false;
    // table_id_extension -> 組み立て中のセクション
    std::map<int, PendingSections> m_pending;
    std::vector<Document> m_documents;
};

// 表示の切り替わり1つ分。文書の時刻(ミリ秒)から次の切り替わりまでcaptionを表示する
struct Segment
{
    int64_t timeMsec = 0;
    aribcaption::Caption caption;
};

// TTMLの文書をlibaribcaptionのCaptionに変換する
class Converter
{
public:
    // component_tagごとに外字(SVGフォント)を登録する。新しく登録した字があればtrue
    bool AddFont(int componentTag, const std::string &svg);
    void Reset();
    // 文書を表示の切り替わりごとのCaptionにする。時刻制御の無い文書は時刻0の1つになる。
    // 解析できなければfalse
    bool Convert(const Document &document, bool superimpose, std::vector<Segment> &segments) const;

private:
    std::map<int, std::unordered_map<uint32_t, aribcaption::DRCS>> m_drcs;
};

// 外字(SVGフォントのグリフ)を描いてDRCSにする。テストからも使う
bool RasterizeGlyph(const std::string &path, double unitsPerEm, double ascent, int size, aribcaption::DRCS &drcs);

// B24の字幕のPES(PES_data_byte)が字幕文か(字幕管理データでないか)
bool IsB24CaptionStatement(const uint8_t *payload, size_t size);

} // namespace TtmlCaption

#endif // INCLUDE_TTML_CAPTION_HPP
