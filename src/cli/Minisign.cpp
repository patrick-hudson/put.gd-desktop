#include "Minisign.h"
#include <QList>
#ifdef PUTGD_MINISIGN_OPENSSL
#include <openssl/evp.h>
#endif
namespace putgd::minisign {
namespace {
Error invalid(const QString& message,const QString& check) {return {"update_signature_invalid",message,{{"check",check}}};}
// Strict base64; empty when the text isn't.
QByteArray base64(const QByteArray& text) {
 auto result=QByteArray::fromBase64Encoding(text.trimmed(),QByteArray::AbortOnBase64DecodingErrors);
 return result?result.decoded:QByteArray{};
}
// The first base64 line that decodes to size bytes starting with one of prefixes.
QByteArray decodedLine(const QByteArray& text,qsizetype size,const QList<QByteArray>& prefixes) {
 for(const auto& raw:text.split('\n')) {
  const auto line=raw.trimmed();
  if(line.isEmpty()||line.startsWith("untrusted comment:")||line.startsWith("trusted comment:"))continue;
  const auto decoded=base64(line);
  if(decoded.size()!=size)continue;
  for(const auto& prefix:prefixes)if(decoded.startsWith(prefix))return decoded;
 }
 return {};
}
#ifdef PUTGD_MINISIGN_OPENSSL
bool ed25519(const QByteArray& publicKey,const QByteArray& signature,const QByteArray& message) {
 auto* key=EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519,nullptr,reinterpret_cast<const unsigned char*>(publicKey.constData()),size_t(publicKey.size()));
 if(!key)return false;
 auto* context=EVP_MD_CTX_new();
 const bool valid=context&&EVP_DigestVerifyInit(context,nullptr,nullptr,nullptr,key)==1&&
  EVP_DigestVerify(context,reinterpret_cast<const unsigned char*>(signature.constData()),size_t(signature.size()),
                   reinterpret_cast<const unsigned char*>(message.constData()),size_t(message.size()))==1;
 EVP_MD_CTX_free(context);EVP_PKEY_free(key);
 return valid;
}
QByteArray blake2b512(const QByteArray& message) {
 QByteArray digest(64,'\0');unsigned int length=0;
 if(EVP_Digest(message.constData(),size_t(message.size()),reinterpret_cast<unsigned char*>(digest.data()),&length,EVP_blake2b512(),nullptr)!=1||length!=64)return {};
 return digest;
}
#endif
}
QByteArray releaseKey(const QString& channel) {
 if(channel=="nightly")return QByteArrayLiteral(PUTGD_MINISIGN_NIGHTLY_KEY);
 if(channel=="stable")return QByteArrayLiteral(PUTGD_MINISIGN_STABLE_KEY);
 return {};
}
bool available() {
#ifdef PUTGD_MINISIGN_OPENSSL
 return true;
#else
 return false;
#endif
}
Error verify(const QByteArray& message,const QByteArray& signatureFile,const QByteArray& publicKeyText,QString* trustedComment) {
 // Public key: "Ed" + key id (8) + Ed25519 key (32).
 const auto publicKey=decodedLine(publicKeyText,42,{"Ed"});
 if(publicKey.isEmpty())return invalid("This build has no usable release key","key");
 // Signature: algorithm ("ED" prehashed, "Ed" legacy) + key id (8) + signature (64),
 // then the trusted comment and its global signature (64).
 const auto lines=signatureFile.split('\n');
 if(lines.size()<4||!lines[0].startsWith("untrusted comment:")||!lines[2].startsWith("trusted comment: "))return invalid("The signature file is malformed","format");
 const auto signature=base64(lines[1]),global=base64(lines[3]);
 if(signature.size()!=74||global.size()!=64)return invalid("The signature file is malformed","format");
 const auto algorithm=signature.left(2);
 if(algorithm!="ED"&&algorithm!="Ed")return invalid("The signature uses an unknown algorithm","algorithm");
 if(signature.mid(2,8)!=publicKey.mid(2,8))return invalid("The signature was made with another key","keyId");
 if(!available())return invalid("This build can't check signatures","unavailable");
#ifdef PUTGD_MINISIGN_OPENSSL
 const auto signed_=algorithm=="ED"?blake2b512(message):message;
 if(signed_.isEmpty()||!ed25519(publicKey.mid(10),signature.mid(10),signed_))return invalid("The signature doesn't match","signature");
 const auto comment=lines[2].mid(int(qstrlen("trusted comment: ")));
 if(!ed25519(publicKey.mid(10),global,signature.mid(10)+comment))return invalid("The signature's trusted comment doesn't match","trustedComment");
 if(trustedComment)*trustedComment=QString::fromUtf8(comment);
#endif
 return {};
}
}
