// The emoji templates (#1748), built into putgd-ocr so it opens no file for
// them: src/ocr/emoji/templates.bin, made by tools/text/emoji_templates.py, on
// pages of their own in read-only data. Only the pages a job reads count in
// its resident set.
#include "Emoji.h"
#include <cstdint>

#ifndef PUTGD_EMOJI_TEMPLATES
#error "PUTGD_EMOJI_TEMPLATES names the template file"
#endif
// The file's SHA-256, so a compiler cache that sees only this source still
// rebuilds it when the file changes.
static const char templatesSha256[] [[maybe_unused]]=PUTGD_EMOJI_TEMPLATES_SHA256;
__asm__(".section .rodata.putgd_emoji,\"a\",@progbits\n"
        ".balign 4096\n"
        ".globl putgd_emoji_templates\n.hidden putgd_emoji_templates\n"
        "putgd_emoji_templates:\n"
        ".incbin \"" PUTGD_EMOJI_TEMPLATES "\"\n"
        ".globl putgd_emoji_templates_end\n.hidden putgd_emoji_templates_end\n"
        "putgd_emoji_templates_end:\n"
        ".balign 4096\n"
        ".previous\n");
extern "C" const uint8_t putgd_emoji_templates[];
extern "C" const uint8_t putgd_emoji_templates_end[];

namespace putgd::ocr {
const EmojiTemplates& EmojiTemplates::builtIn(){
 static const EmojiTemplates templates=[]{
  EmojiTemplates loaded;
  if(!loaded.load(putgd_emoji_templates,putgd_emoji_templates_end-putgd_emoji_templates))loaded={};
  else loaded.mapped_=true;
  return loaded;
 }();
 return templates;
}
}
