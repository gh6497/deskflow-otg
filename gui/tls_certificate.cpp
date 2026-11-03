// SPDX-License-Identifier: Apache-2.0
#include "tls_certificate.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSslCertificate>
#include <QSslKey>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <memory>

namespace TlsCertificate {
namespace {

bool fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}

void freeBio(BIO *bio)
{
    BIO_free(bio);
}

template<typename T, void (*Free)(T *)>
struct OpenSslDeleter {
    void operator()(T *value) const
    {
        if (value) Free(value);
    }
};

template<typename T, void (*Free)(T *)>
using OpenSslPtr = std::unique_ptr<T, OpenSslDeleter<T, Free>>;

} // namespace

bool generate(const QString &path, int bits, QString *error)
{
    if (error) error->clear();
    if (path.trimmed().isEmpty())
        return fail(error, QStringLiteral("certificate path is empty"));
    if (bits != 2048 && bits != 4096)
        return fail(error, QStringLiteral("unsupported RSA key length"));

    const QFileInfo info(path);
    QDir directory(info.absolutePath());
    if (!directory.exists() && !directory.mkpath(QStringLiteral(".")))
        return fail(error, QStringLiteral("could not create the certificate directory"));

    OpenSslPtr<EVP_PKEY_CTX, EVP_PKEY_CTX_free> keyContext(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr));
    if (!keyContext || EVP_PKEY_keygen_init(keyContext.get()) != 1 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(keyContext.get(), bits) != 1)
        return fail(error, QStringLiteral("could not initialize RSA key generation"));

    EVP_PKEY *rawKey = nullptr;
    if (EVP_PKEY_keygen(keyContext.get(), &rawKey) != 1 || !rawKey)
        return fail(error, QStringLiteral("could not generate the RSA key"));
    OpenSslPtr<EVP_PKEY, EVP_PKEY_free> key(rawKey);

    OpenSslPtr<X509, X509_free> certificate(X509_new());
    if (!certificate)
        return fail(error, QStringLiteral("could not allocate the certificate"));
    if (X509_set_version(certificate.get(), 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) != 1 ||
        !X509_gmtime_adj(X509_get_notBefore(certificate.get()), 0) ||
        !X509_gmtime_adj(X509_get_notAfter(certificate.get()), 365 * 24 * 60 * 60) ||
        X509_set_pubkey(certificate.get(), key.get()) != 1)
        return fail(error, QStringLiteral("could not initialize the certificate"));

    auto *name = X509_get_subject_name(certificate.get());
    if (!name || X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                            reinterpret_cast<const unsigned char *>("Deskflow OTG Client"), -1, -1, 0) != 1 ||
        X509_set_issuer_name(certificate.get(), name) != 1 ||
        X509_sign(certificate.get(), key.get(), EVP_sha256()) <= 0)
        return fail(error, QStringLiteral("could not sign the certificate"));

    OpenSslPtr<BIO, freeBio> memory(BIO_new(BIO_s_mem()));
    if (!memory || PEM_write_bio_PrivateKey(memory.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1 ||
        PEM_write_bio_X509(memory.get(), certificate.get()) != 1)
        return fail(error, QStringLiteral("could not encode the certificate"));

    BUF_MEM *buffer = nullptr;
    BIO_get_mem_ptr(memory.get(), &buffer);
    if (!buffer || !buffer->data || buffer->length <= 0)
        return fail(error, QStringLiteral("could not read the encoded certificate"));

    const QByteArray pem(buffer->data, static_cast<int>(buffer->length));
    QSaveFile output(path);
    // Set private permissions on the temporary file before writing any key data.
    // Keep QSaveFile's atomic rename; a failed save preserves the old PEM.
    if (!output.open(QIODevice::WriteOnly) ||
        !output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        output.write(pem) != pem.size() || !output.commit())
        return fail(error, output.errorString().isEmpty() ? QStringLiteral("could not write the certificate") :
                                                            output.errorString());

    return true;
}

QString fingerprint(const QString &path)
{
    const auto certificates = QSslCertificate::fromPath(path, QSsl::Pem);
    if (certificates.isEmpty() || certificates.first().isNull())
        return {};
    return certificates.first().digest(QCryptographicHash::Sha256).toHex(':').toUpper();
}

int keyLength(const QString &path)
{
    const auto certificates = QSslCertificate::fromPath(path, QSsl::Pem);
    if (certificates.isEmpty() || certificates.first().isNull())
        return -1;
    const auto key = certificates.first().publicKey();
    return key.isNull() ? -1 : key.length();
}

} // namespace TlsCertificate
