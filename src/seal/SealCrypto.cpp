#include "SealCrypto.h"
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QtEndian>
#include <cmath>
#include <memory>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
namespace putgd::seal {
namespace {
constexpr char magic[]="PGDSEAL1";
const QByteArray checkInfo="put.gd sealed v1 check";
const QByteArray payloadInfo="put.gd sealed v1 payload";
Error damaged() {return {"seal_damaged","This link is incomplete or damaged.",{}};}
Error failed(const QString& what) {return {"seal_failed","Couldn't seal the file: "+what+".",{}};}
const unsigned char* bytes(const QByteArray& value) {return reinterpret_cast<const unsigned char*>(value.constData());}
// HKDF-SHA-256 through EVP_PKEY, available from OpenSSL 1.1.1 on.
QByteArray hkdf(const QByteArray& ikm,const QByteArray& salt,const QByteArray& info) {
 std::unique_ptr<EVP_PKEY_CTX,decltype(&EVP_PKEY_CTX_free)> context(EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF,nullptr),EVP_PKEY_CTX_free);
 QByteArray out(keyBytes,'\0');size_t length=size_t(out.size());
 if(!context||EVP_PKEY_derive_init(context.get())<=0||EVP_PKEY_CTX_set_hkdf_md(context.get(),EVP_sha256())<=0
    ||EVP_PKEY_CTX_set1_hkdf_salt(context.get(),bytes(salt),int(salt.size()))<=0
    ||EVP_PKEY_CTX_set1_hkdf_key(context.get(),bytes(ikm),int(ikm.size()))<=0
    ||EVP_PKEY_CTX_add1_hkdf_info(context.get(),bytes(info),int(info.size()))<=0
    ||EVP_PKEY_derive(context.get(),reinterpret_cast<unsigned char*>(out.data()),&length)<=0||length!=size_t(keyBytes))return {};
 return out;
}
QByteArray hmac(const QByteArray& key,const QByteArray& message) {
 QByteArray out(32,'\0');unsigned int length=0;
 if(!HMAC(EVP_sha256(),key.constData(),int(key.size()),bytes(message),size_t(message.size()),reinterpret_cast<unsigned char*>(out.data()),&length)||length!=32)return {};
 return out;
}
// The passphrase as both sides derive from it: NFC, then UTF-8.
QByteArray passphraseBytes(const QString& passphrase) {return passphrase.normalized(QString::NormalizationForm_C).toUtf8();}
QByteArray passphraseKey(const QString& passphrase,const QByteArray& salt,quint32 iterations) {
 auto text=passphraseBytes(passphrase);
 QByteArray out(keyBytes,'\0');
 const bool ok=PKCS5_PBKDF2_HMAC(text.constData(),int(text.size()),bytes(salt),int(salt.size()),int(iterations),EVP_sha256(),keyBytes,reinterpret_cast<unsigned char*>(out.data()))==1;
 OPENSSL_cleanse(text.data(),size_t(text.size()));
 if(!ok){OPENSSL_cleanse(out.data(),size_t(out.size()));return {};}
 return out;
}
// One line of 1 to 1024 bytes once normalized: what the viewer's field and putgd-cli open can take.
bool usablePassphrase(const QString& passphrase) {
 if(passphrase.contains('\n')||passphrase.contains('\r'))return false;
 auto text=passphraseBytes(passphrase);const bool ok=text.size()<=maximumPassphraseBytes;OPENSSL_cleanse(text.data(),size_t(text.size()));return ok;
}
QByteArray keyCheck(const QByteArray& key,const QByteArray& salt,const QByteArray& head) {
 const auto check=hkdf(key,salt,checkInfo);return check.isEmpty()?QByteArray():hmac(check,head);
}
QByteArray fileKey(const QByteArray& key,const Header& header,const QString& passphrase) {
 auto ikm=key;
 if(header.passphrase) {auto derived=passphraseKey(passphrase,header.salt,header.iterations);if(derived.isEmpty()){OPENSSL_cleanse(ikm.data(),size_t(ikm.size()));return {};}ikm+=derived;OPENSSL_cleanse(derived.data(),size_t(derived.size()));}
 auto derived=hkdf(ikm,header.salt,payloadInfo);OPENSSL_cleanse(ikm.data(),size_t(ikm.size()));
 return derived;
}
// The chunk's nonce tail and additional data tail: u32 BE index || u8 last.
QByteArray position(quint32 index,bool last) {QByteArray out(5,'\0');qToBigEndian(index,out.data());out[4]=last?1:0;return out;}
struct Cipher {
 std::unique_ptr<EVP_CIPHER_CTX,decltype(&EVP_CIPHER_CTX_free)> context{EVP_CIPHER_CTX_new(),EVP_CIPHER_CTX_free};
 QByteArray key,header,prefix;
 // Encrypts or decrypts one chunk; for decryption the tag is the input's last 16 bytes.
 bool run(bool encrypt,quint32 index,bool last,const char* in,qint64 size,QByteArray& out) {
  if(!context)return false;
  const auto tail=position(index,last);const auto nonce=prefix+tail;const auto aad=header+tail;
  const qint64 body=encrypt?size:size-tagBytes;if(body<0)return false;
  out.resize(encrypt?body+tagBytes:body);int length=0;
  auto* cipherOut=reinterpret_cast<unsigned char*>(out.data());const auto* data=reinterpret_cast<const unsigned char*>(in);
  if(encrypt) {
   if(EVP_EncryptInit_ex(context.get(),EVP_aes_256_gcm(),nullptr,nullptr,nullptr)!=1||EVP_CIPHER_CTX_ctrl(context.get(),EVP_CTRL_GCM_SET_IVLEN,12,nullptr)!=1
      ||EVP_EncryptInit_ex(context.get(),nullptr,nullptr,bytes(key),bytes(nonce))!=1
      ||EVP_EncryptUpdate(context.get(),nullptr,&length,bytes(aad),int(aad.size()))!=1
      ||(body>0&&EVP_EncryptUpdate(context.get(),cipherOut,&length,data,int(body))!=1)
      ||EVP_EncryptFinal_ex(context.get(),cipherOut+body,&length)!=1
      ||EVP_CIPHER_CTX_ctrl(context.get(),EVP_CTRL_GCM_GET_TAG,tagBytes,cipherOut+body)!=1)return false;
   return true;
  }
  QByteArray tag(in+body,tagBytes);
  const bool ok=EVP_DecryptInit_ex(context.get(),EVP_aes_256_gcm(),nullptr,nullptr,nullptr)==1&&EVP_CIPHER_CTX_ctrl(context.get(),EVP_CTRL_GCM_SET_IVLEN,12,nullptr)==1
      &&EVP_DecryptInit_ex(context.get(),nullptr,nullptr,bytes(key),bytes(nonce))==1
      &&EVP_DecryptUpdate(context.get(),nullptr,&length,bytes(aad),int(aad.size()))==1
      &&(body==0||EVP_DecryptUpdate(context.get(),cipherOut,&length,data,int(body))==1)
      &&EVP_CIPHER_CTX_ctrl(context.get(),EVP_CTRL_GCM_SET_TAG,tagBytes,tag.data())==1
      &&EVP_DecryptFinal_ex(context.get(),cipherOut+body,&length)==1;
  if(!ok)OPENSSL_cleanse(out.data(),size_t(out.size()));
  return ok;
 }
};
// The meta record exactly as the viewer accepts it: an object with string name and type and a
// whole size from 0 to the sealing limit.
bool parseMeta(const QByteArray& json,Meta& meta) {
 QJsonParseError error;const auto document=QJsonDocument::fromJson(json,&error);
 if(error.error!=QJsonParseError::NoError||!document.isObject())return false;
 const auto object=document.object();const auto size=object.value("size");
 if(!object.value("name").isString()||!object.value("type").isString()||!size.isDouble())return false;
 const double value=size.toDouble();if(value<0||value>double(sealLimit)||value!=std::floor(value))return false;
 meta={object.value("name").toString(),object.value("type").toString(),qint64(value)};return true;
}
bool readExactly(QIODevice& input,char* into,qint64 size) {
 qint64 done=0;
 while(done<size){const auto got=input.read(into+done,size-done);if(got<=0){if(got==0&&input.waitForReadyRead(30000))continue;return false;}done+=got;}
 return true;
}
}
QString fragment(const QByteArray& key) {return "v1."+QString::fromLatin1(key.toBase64(QByteArray::Base64UrlEncoding|QByteArray::OmitTrailingEquals));}
QByteArray keyFromFragment(QString text) {
 if(text.startsWith('#'))text.remove(0,1);
 if(!text.startsWith("v1.")||text.size()!=46)return {};
 const auto body=text.mid(3).toLatin1();
 for(const char c:body)if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'))return {};
 auto decoded=QByteArray::fromBase64Encoding(body,QByteArray::Base64UrlEncoding|QByteArray::AbortOnBase64DecodingErrors);
 // The 43rd character carries 4 unused bits; only the canonical form is a key.
 if(!decoded||decoded.decoded.size()!=keyBytes||fragment(decoded.decoded)!=text)return {};
 return decoded.decoded;
}
QByteArray randomKey() {QByteArray key(keyBytes,'\0');if(RAND_bytes(reinterpret_cast<unsigned char*>(key.data()),keyBytes)!=1)return {};return key;}
qint64 containerBytes(qint64 plaintext,qint64 chunkSize) {
 if(plaintext<=0)return headerBytes;
 const qint64 chunks=(plaintext+chunkSize-1)/chunkSize;return headerBytes+plaintext+chunks*tagBytes;
}
QByteArray encodeMeta(const Meta& meta) {
 const auto json=QJsonDocument(QJsonObject{{"name",meta.name},{"type",meta.type},{"size",double(meta.size)}}).toJson(QJsonDocument::Compact);
 QByteArray out(4,'\0');qToBigEndian(quint32(json.size()),out.data());return out+json;
}
Error seal(QIODevice& input,const Meta& meta,const QByteArray& key,const QString& passphrase,const Sink& sink,int iterations,const Fixed& fixed) {
 if(key.size()!=keyBytes)return failed("the key is invalid");
 if(meta.size<0||meta.size>sealLimit)return failed("the file is too large");
 if(!passphrase.isEmpty()&&!usablePassphrase(passphrase))return failed("the passphrase must be one line of at most 1024 bytes");
 const auto encodedMeta=encodeMeta(meta);if(encodedMeta.size()>maximumMetaBytes+4)return failed("the file name is too long");
 Header header;header.passphrase=!passphrase.isEmpty();header.chunkSize=quint32(chunkBytes);header.iterations=header.passphrase?quint32(passphraseIterations):0;Q_UNUSED(iterations);
 header.salt=fixed.salt.size()==32?fixed.salt:QByteArray(32,'\0');header.noncePrefix=fixed.noncePrefix.size()==7?fixed.noncePrefix:QByteArray(7,'\0');
 if(fixed.salt.size()!=32&&RAND_bytes(reinterpret_cast<unsigned char*>(header.salt.data()),32)!=1)return failed("no random bytes");
 if(fixed.noncePrefix.size()!=7&&RAND_bytes(reinterpret_cast<unsigned char*>(header.noncePrefix.data()),7)!=1)return failed("no random bytes");
 QByteArray head(magic,8);head.append(char(1));head.append(char(header.passphrase?1:0));head.append(2,'\0');
 {char word[4];qToBigEndian(header.chunkSize,word);head.append(word,4);qToBigEndian(header.iterations,word);head.append(word,4);}
 head+=header.salt;head+=header.noncePrefix;head.append(5,'\0');
 const auto check=keyCheck(key,header.salt,head);if(check.isEmpty())return failed("key derivation failed");
 header.bytes=head+check;
 Cipher cipher;cipher.key=fileKey(key,header,passphrase);cipher.header=header.bytes;cipher.prefix=header.noncePrefix;
 if(cipher.key.isEmpty())return failed("key derivation failed");
 const auto clean=qScopeGuard([&cipher]{OPENSSL_cleanse(cipher.key.data(),size_t(cipher.key.size()));});
 if(!sink(header.bytes.constData(),header.bytes.size()))return failed("couldn't write the sealed file");
 const qint64 total=encodedMeta.size()+meta.size;
 QByteArray plain;plain.reserve(int(chunkBytes));QByteArray out;
 const auto wipe=qScopeGuard([&plain]{plain.resize(plain.capacity());OPENSSL_cleanse(plain.data(),size_t(plain.size()));});
 qint64 consumed=0,fileRead=0;quint32 index=0;
 while(consumed<total) {
  plain.clear();
  const qint64 want=qMin<qint64>(chunkBytes,total-consumed);
  if(consumed<encodedMeta.size()){const auto take=qMin<qint64>(want,encodedMeta.size()-consumed);plain.append(encodedMeta.constData()+consumed,take);}
  const qint64 fromFile=want-plain.size();
  if(fromFile>0){const auto at=plain.size();plain.resize(at+fromFile);if(!readExactly(input,plain.data()+at,fromFile))return failed("couldn't read the file");fileRead+=fromFile;}
  consumed+=want;const bool last=consumed==total;
  if(!cipher.run(true,index,last,plain.constData(),plain.size(),out))return failed("encryption failed");
  if(!sink(out.constData(),out.size()))return failed("couldn't write the sealed file");
  if(index==0xffffffffu&&!last)return failed("the file is too large");
  ++index;
 }
 return {};
}
Error parseHeader(const QByteArray& value,Header& header) {
 if(value.size()!=headerBytes||!value.startsWith(QByteArray(magic,8)))return damaged();
 if(value[8]!=1)return {"seal_version","This sealed link needs a newer put.gd.",{}};
 const auto flags=quint8(value[9]);if(flags>1||value[10]!=0||value[11]!=0)return damaged();
 header.passphrase=flags&1;header.chunkSize=qFromBigEndian<quint32>(value.constData()+12);header.iterations=qFromBigEndian<quint32>(value.constData()+16);
 // Version 1 fixes the chunk size and the passphrase cost: anything else is not ours.
 if(header.chunkSize!=quint32(chunkBytes))return damaged();
 // Version 1 fixes the passphrase cost at 600,000 iterations.
 if(header.passphrase?header.iterations!=quint32(passphraseIterations):header.iterations!=0)return damaged();
 for(int i=59;i<64;++i)if(value[i]!=0)return damaged();
 header.salt=value.mid(20,32);header.noncePrefix=value.mid(52,7);header.bytes=value;
 return {};
}
bool keyMatches(const Header& header,const QByteArray& key) {
 if(key.size()!=keyBytes)return false;
 const auto check=keyCheck(key,header.salt,header.bytes.left(64));
 return check.size()==32&&CRYPTO_memcmp(check.constData(),header.bytes.constData()+64,32)==0;
}
Error open(QIODevice& input,qint64 total,const QByteArray& key,const QString& passphrase,const Sink& sink,Meta* meta) {
 if(total<headerBytes+tagBytes+1)return damaged();
 QByteArray head(headerBytes,'\0');if(!readExactly(input,head.data(),headerBytes))return damaged();
 Header header;if(auto e=parseHeader(head,header))return e;
 if(!keyMatches(header,key))return damaged();
 if(header.passphrase&&passphrase.isEmpty())return {"seal_passphrase_required","This link needs its passphrase.",{}};
 if(!usablePassphrase(passphrase))return {"seal_passphrase","That passphrase doesn't open this link.",{}};
 Cipher cipher;cipher.key=fileKey(key,header,header.passphrase?passphrase:QString());cipher.header=header.bytes;cipher.prefix=header.noncePrefix;
 if(cipher.key.isEmpty())return damaged();
 const auto clean=qScopeGuard([&cipher]{OPENSSL_cleanse(cipher.key.data(),size_t(cipher.key.size()));});
 const qint64 body=total-headerBytes,full=qint64(header.chunkSize)+tagBytes;
 const qint64 chunks=(body+full-1)/full,lastSize=body-(chunks-1)*full;
 if(lastSize<tagBytes+1||chunks>0x100000000LL)return damaged();
 QByteArray in,out,metaBuffer;qint64 metaLength=-1;qint64 fileBytes=0;
 // Every way out wipes what was decrypted here.
 // Each buffer's whole capacity: a shorter last chunk leaves earlier plaintext past its size.
 const auto wipe=qScopeGuard([&]{for(auto* buffer:{&in,&out,&metaBuffer})if(buffer->capacity()>0){buffer->resize(buffer->capacity());OPENSSL_cleanse(buffer->data(),size_t(buffer->size()));}});
 for(qint64 index=0;index<chunks;++index) {
  const bool last=index==chunks-1;const auto size=last?lastSize:full;
  in.resize(size);if(!readExactly(input,in.data(),size))return damaged();
  if(!cipher.run(false,quint32(index),last,in.constData(),size,out))
   return index==0&&header.passphrase?Error{"seal_passphrase","That passphrase doesn't open this link.",{}}:damaged();
  qint64 offset=0;
  if(metaLength<0||metaBuffer.size()<4+metaLength) {
   // The meta record comes first and may, in principle, cross a chunk boundary.
   const auto need=metaLength<0?4-metaBuffer.size():4+metaLength-metaBuffer.size();
   const auto take=qMin<qint64>(need,out.size());metaBuffer.append(out.constData(),take);offset=take;
   if(metaLength<0&&metaBuffer.size()==4){metaLength=qFromBigEndian<quint32>(metaBuffer.constData());if(metaLength>maximumMetaBytes)return damaged();
    const auto more=qMin<qint64>(metaLength,out.size()-offset);metaBuffer.append(out.constData()+offset,more);offset+=more;}
   if(metaLength>=0&&metaBuffer.size()==4+metaLength) {
    Meta parsed;if(!parseMeta(metaBuffer.mid(4),parsed))return damaged();
    if(meta)*meta=parsed;
   }
  }
  if(offset<out.size()){fileBytes+=out.size()-offset;if(!sink(out.constData()+offset,out.size()-offset))return {"seal_write_failed","Couldn't write the opened file.",{}};}
 }
 if(metaLength<0||metaBuffer.size()!=4+metaLength)return damaged();
 Meta parsed;if(!parseMeta(metaBuffer.mid(4),parsed)||parsed.size!=fileBytes)return damaged();
 return {};
}
}
