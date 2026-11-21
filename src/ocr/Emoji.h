#pragma once
#include <QList>
#include <QRect>
#include <QString>
#include <cstdint>
#include <functional>

// Emoji on screen (#1748). Tesseract's models hold no emoji, so it reads one as
// stray letters or symbols. The helper finds colour glyphs in the input's colour
// plane and names each one by matching it against templates drawn from Noto
// Color Emoji and Twemoji (tools/text/emoji_templates.py). Qt Core only.
namespace putgd::ocr {
// 8-bit RGB rows, flattened onto white. done(top, bottom) is told when the
// scan has finished with rows [top, bottom), so a mapped plane can let them go.
struct ColourView {
 const uint8_t* data=nullptr;
 int width=0,height=0;
 qsizetype stride=0;
 std::function<void(int,int)> done;
 bool valid()const{return data&&width>0&&height>0&&stride>=qsizetype(width)*3;}
 const uint8_t* row(int y)const{return data+qsizetype(y)*stride;}
};

// The generated template file: entries, UTF-8 sequences, and each glyph's ink
// box stretched to coarse x coarse and side x side premultiplied RGBA pixels.
class EmojiTemplates {
public:
 // False unless `bytes` is a whole, consistent template file. The data must
 // outlive this object; nothing is copied.
 bool load(const uint8_t* data,qsizetype bytes);
 int count()const{return count_;}
 int side()const{return side_;}
 int coarseSide()const{return coarseSide_;}
 QString text(int index)const;
 int style(int index)const;
 double aspect(int index)const;   // ink box width over height
 double colour(int index)const;   // the share of its opaque pixels that are colour
 // A plain shape (a coloured circle or square, a keycap, a dash): matched, so
 // a status dot is not taken for something else, but never named.
 bool shape(int index)const;
 const uint8_t* pixels(int index)const{return pixels_+qsizetype(index)*side_*side_*4;}
 const uint8_t* coarse(int index)const{return coarse_+qsizetype(index)*coarseSide_*coarseSide_*4;}
 // The templates built into the helper.
 static const EmojiTemplates& builtIn();
 // Lets the pages of the built-in templates go once a job is done with them
 // (they are part of the executable and come back if read again). Does
 // nothing for templates loaded from memory.
 void release()const;
private:
 const uint8_t* data_=nullptr;
 const uint8_t* entries_=nullptr;
 const uint8_t* strings_=nullptr;
 const uint8_t* coarse_=nullptr;
 const uint8_t* pixels_=nullptr;
 int count_=0,side_=0,coarseSide_=0;
 qsizetype stringBytes_=0,bytes_=0;
 bool mapped_=false;   // the built-in data, pages of the executable
};

struct EmojiOptions {
 // A glyph is named when the best template's distance (mean absolute colour
 // difference over the glyph, 0 to 1) is at most this.
 double distance=0.09;
 // ...and at most this share of the template's contrast with the background
 // (its mean colour distance from the background over its opaque pixels): a
 // dark template on a dark background, or a pale one on white, stands out too
 // little to be told from a faint coloured letter.
 double relativeDistance=0.25;
 int shortlist=24;           // templates rescored at the glyph's own size
 // A glyph whose closest template, both stretched to the template's size,
 // differs by more than this is not rescored at its own size.
 double stretchedDistance=0.18;
 int coarseShortlist=64;     // templates compared at full template size
 int maximumCandidates=512;  // colour glyphs examined in one image
 int minimumSize=10,maximumSize=160;  // a glyph's larger side, pixels
 // A template whose share of colour pixels differs from the glyph's by more
 // than this is not considered: a blue letter is not a white tooth.
 double colourShare=0.35;
 bool keepCandidates=false;  // measurements: fill EmojiScan::examined
};
struct EmojiHit {
 QString text;
 QRect rect;         // the glyph, source pixels
 double distance=0;
 int style=0;
};
// A colour glyph the scan matched, for measurements: its box, the share of
// it that is colour core, and the best distance found.
struct EmojiCandidate { QRect rect;double fill=0,distance=9,stretched=9; };
struct EmojiScan {
 QList<EmojiHit> hits;   // top to bottom, then left to right
 int candidates=0;
 bool cut=false;         // more colour glyphs than maximumCandidates, or stopped
 QList<EmojiCandidate> examined;  // only with EmojiOptions::keepCandidates
};
// stop is asked between candidates; true ends the scan with what it has.
EmojiScan findEmoji(const ColourView& image,const EmojiTemplates& templates,const EmojiOptions& options={},
                    const std::function<bool()>& stop={});
}
