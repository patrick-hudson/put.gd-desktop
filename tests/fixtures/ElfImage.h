#pragma once
#include <QByteArray>
#include <QtEndian>
namespace putgd::test {
// A little-endian ELF64 file with the AppImage type 2 magic and a .upd_info
// section, laid out as the runtime lays it out; no code. `payload` follows the
// section table, so versions of the same image differ.
inline QByteArray elfImage(const QByteArray& updateInformation,const QByteArray& payload={}) {
 const QByteArray names("\0.shstrtab\0.upd_info\0",21);
 auto info=updateInformation;info.append(QByteArray(1024-info.size(),'\0'));
 QByteArray header(64,'\0');
 header.replace(0,4,"\x7f""ELF");header[4]=2;header[5]=1;header[6]=1;header[8]='A';header[9]='I';header[10]=2;
 const quint64 tableOffset=64+names.size()+info.size();
 qToLittleEndian<quint64>(tableOffset,header.data()+0x28);
 qToLittleEndian<quint16>(64,header.data()+0x3A);qToLittleEndian<quint16>(3,header.data()+0x3C);qToLittleEndian<quint16>(1,header.data()+0x3E);
 QByteArray table(3*64,'\0');
 const auto section=[&](int index,quint32 name,quint64 offset,quint64 size){
  qToLittleEndian<quint32>(name,table.data()+index*64);qToLittleEndian<quint64>(offset,table.data()+index*64+24);qToLittleEndian<quint64>(size,table.data()+index*64+32);
 };
 section(1,1,64,names.size());section(2,11,64+names.size(),info.size());
 return header+names+info+table+payload;
}
}
