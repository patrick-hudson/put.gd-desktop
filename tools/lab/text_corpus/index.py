#!/usr/bin/env python3
"""Add transcript counts to a corpus index, without changing images or text.

Raw characters are Unicode code points including newlines. Normalized characters
use the scorer's NFC and whitespace collapsing. Both are recorded so a latency
receipt can identify how much text its source contains. No acceptance verdict.
"""
import argparse
import json
from pathlib import Path
import unicodedata


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    document=json.loads(args.manifest.read_text())
    for capture in document['captures']:
        if 'transcript_codepoints' in capture:
            text=''.join(map(chr,json.loads((args.manifest.parent/capture['transcript_codepoints']).read_text())))
        else:
            text=(args.manifest.parent/capture['transcript']).read_text()
        normalized=' '.join(unicodedata.normalize('NFC',text).split())
        capture.update(characters=len(text),normalized_characters=len(normalized),words=len(normalized.split()))
    document['character_counts']='Unicode code points; raw includes newlines; normalized is NFC with collapsed whitespace.'
    with args.output.open('x') as stream:
        json.dump(document,stream,indent=2);stream.write('\n')


if __name__=='__main__':main()
