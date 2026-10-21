#pragma once
#include <QByteArray>
#include <openssl/evp.h>
// A throwaway minisign key that signs the way the release signer does
// (#293 phase 3): prehashed "ED" by default, or legacy "Ed".
class MinisignKey {
public:
 explicit MinisignKey(const QByteArray& keyId=QByteArray::fromHex("0102030405060708")):keyId_(keyId),key_(EVP_PKEY_Q_keygen(nullptr,nullptr,"ED25519")) {
  size_t length=32;publicKey_.resize(32);
  EVP_PKEY_get_raw_public_key(key_,reinterpret_cast<unsigned char*>(publicKey_.data()),&length);
 }
 ~MinisignKey() {EVP_PKEY_free(key_);}
 MinisignKey(const MinisignKey&)=delete;MinisignKey& operator=(const MinisignKey&)=delete;
 QByteArray publicFile() const {return "untrusted comment: minisign public key "+keyId_.toHex().toUpper()+"\n"+(QByteArray("Ed")+keyId_+publicKey_).toBase64()+"\n";}
 QByteArray signature(const QByteArray& message,const QByteArray& trusted,bool prehashed=true) const {
  const auto signature=raw(prehashed?blake2b(message):message);
  return "untrusted comment: signature from minisign secret key\n"+(QByteArray(prehashed?"ED":"Ed")+keyId_+signature).toBase64()+
         "\ntrusted comment: "+trusted+"\n"+raw(signature+trusted).toBase64()+"\n";
 }
private:
 static QByteArray blake2b(const QByteArray& message) {
  QByteArray digest(64,'\0');unsigned int length=0;
  EVP_Digest(message.constData(),size_t(message.size()),reinterpret_cast<unsigned char*>(digest.data()),&length,EVP_blake2b512(),nullptr);
  return digest;
 }
 QByteArray raw(const QByteArray& message) const {
  QByteArray signature(64,'\0');size_t length=64;
  auto* context=EVP_MD_CTX_new();
  EVP_DigestSignInit(context,nullptr,nullptr,nullptr,key_);
  EVP_DigestSign(context,reinterpret_cast<unsigned char*>(signature.data()),&length,reinterpret_cast<const unsigned char*>(message.constData()),size_t(message.size()));
  EVP_MD_CTX_free(context);
  return signature;
 }
 QByteArray keyId_,publicKey_;
 EVP_PKEY* key_=nullptr;
};
