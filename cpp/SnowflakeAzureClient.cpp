/*
 * Copyright (c) 2019 Snowflake Computing, Inc. All rights reserved.
 */

#include "SnowflakeAzureClient.hpp"
#include "FileMetadataInitializer.hpp"
#include "snowflake/client.h"
#include "util/Base64.hpp"
#include "util/ByteArrayStreamBuf.hpp"
#include "util/Proxy.hpp"
#include "crypto/CipherStreamBuf.hpp"
#include "logger/SFAwsLogger.hpp"
#include "logger/SFLogger.hpp"
#include "SnowflakeS3Client.hpp"
#include "storage_credential.h"
#include "storage_account.h"
#include "blob/blob_client.h"
#include <algorithm>
#include <iostream>
#include <fstream>
#include <string>



#define CONTENT_TYPE_OCTET_STREAM "application/octet-stream"
#define SFC_DIGEST "sfc-digest"
#define AMZ_KEY "x-amz-key"
#define AMZ_IV "x-amz-iv"
#define AMZ_MATDESC "x-amz-matdesc"

namespace Snowflake
{
namespace Client
{


SnowflakeAzureClient::SnowflakeAzureClient(StageInfo *stageInfo, unsigned int parallel,
  TransferConfig * transferConfig):
  m_stageInfo(stageInfo),
  m_threadPool(nullptr),
  m_parallel(std::min(parallel, std::thread::hardware_concurrency()))
{
  const std::string azuresaskey("AZURE_SAS_KEY");
  //TODO: Read the CAPATH from configuration. 
  std::string capath="/etc/pki/tls/certs";

  std::string account_name = m_stageInfo->storageAccount;
  std::string sas_key = m_stageInfo->credentials[azuresaskey];
  std::string endpoint = account_name + "." + m_stageInfo->endPoint;
  std::shared_ptr<azure::storage_lite::storage_credential>  cred = std::make_shared<azure::storage_lite::shared_access_signature_credential>(sas_key);
  std::shared_ptr<azure::storage_lite::storage_account> account = std::make_shared<azure::storage_lite::storage_account>(account_name, cred, true, endpoint);
  auto bc = std::make_shared<azure::storage_lite::blob_client>(account, parallel, capath.c_str());
  m_blobclient= new azure::storage_lite::blob_client_wrapper(bc);

  CXX_LOG_TRACE("Successfully created Azure client. End of constructor.");
}

SnowflakeAzureClient::~SnowflakeAzureClient()
{
}

RemoteStorageRequestOutcome SnowflakeAzureClient::upload(FileMetadata *fileMetadata,
                                          std::basic_iostream<char> *dataStream)
{
    return doSingleUpload(fileMetadata, dataStream);
}

RemoteStorageRequestOutcome SnowflakeAzureClient::doSingleUpload(FileMetadata *fileMetadata,
  std::basic_iostream<char> *dataStream)
{
  CXX_LOG_DEBUG("Start single part upload for file %s",
               fileMetadata->srcFileToUpload.c_str());

  std::string containerName = m_stageInfo->location;

  //Remove the trailing '/' in containerName
  containerName.pop_back();

  std::string blobName = fileMetadata->destFileName;

  //metadata azure uses.
  std::vector<std::pair<std::string, std::string>> userMetadata;
  addUserMetadata(&userMetadata, fileMetadata);
  //Calculate the length of the stream.
  unsigned int len = (unsigned int) (fileMetadata->encryptionMetadata.cipherStreamSize > 0) ? fileMetadata->encryptionMetadata.cipherStreamSize: fileMetadata->srcFileToUploadSize ;

  //Azure does not provide to SHA256 or MD5 or checksum check of a file to check if it already exists.
  bool exists = m_blobclient->blob_exists(containerName, blobName);
  if(exists)
  {
      return RemoteStorageRequestOutcome::SKIP_UPLOAD_FILE;
  }
  m_blobclient->upload_block_blob_from_stream(containerName, blobName, *dataStream, userMetadata, len);
  if (errno != 0)
      return RemoteStorageRequestOutcome::FAILED;

  return RemoteStorageRequestOutcome::SUCCESS;
}

void Snowflake::Client::SnowflakeAzureClient::uploadParts(MultiUploadCtx_a * uploadCtx)
{

}

RemoteStorageRequestOutcome SnowflakeAzureClient::doMultiPartUpload(FileMetadata *fileMetadata,
  std::basic_iostream<char> *dataStream)
{
  CXX_LOG_DEBUG("Start multi part upload for file %s",
               fileMetadata->srcFileToUpload.c_str());

}

std::string buildEncryptionMetadataJSON(std::string iv64, std::string key64)
{
  char buf[1024];
 // sprintf(buf,"{\"EncryptionMode\":\"FullBlob\",\"WrappedContentKey\":{\"KeyId\":\"symmKey1\",\"EncryptedKey\":\"%s\",\"Algorithm\":\"AES_CBC_256\"},\"EncryptionAgent\":{\"Protocol\":\"1.0\",\"EncryptionAlgorithm\":\"AES_CBC_256\"},\"ContentEncryptionIV\":\"%s\", \"KeyWrappingMetadata\":{\"EncryptionLibrary\":\"Java 5.3.0\"}}", key64.c_str(), iv64.c_str());
  sprintf(buf,"{\"EncryptionMode\":\"FullBlob\",\"WrappedContentKey\":{\"KeyId\":\"symmKey1\",\"EncryptedKey\":\"%s\",\"Algorithm\":\"AES256\"},\"EncryptionAgent\":{\"Protocol\":\"1.0\",\"EncryptionAlgorithm\":\"AES256\"},\"ContentEncryptionIV\":\"%s\"}", key64.c_str(), iv64.c_str());
  printf("String length of Encryption metadata is %lu", strlen(buf));

  return std::string(buf);
}

void SnowflakeAzureClient::addUserMetadata(std::vector<std::pair<std::string, std::string>> *userMetadata, FileMetadata *fileMetadata)
{
  //userMetadata->push_back(std::make_pair("key", fileMetadata->encryptionMetadata.enKekEncoded));

  userMetadata->push_back(std::make_pair("matdesc", fileMetadata->encryptionMetadata.matDesc));

  char ivEncoded[64];
  Snowflake::Client::Util::Base64::encode(
          fileMetadata->encryptionMetadata.iv.data,
          Crypto::cryptoAlgoBlockSize(Crypto::CryptoAlgo::AES),
          ivEncoded);

  size_t ivEncodeSize = Snowflake::Client::Util::Base64::encodedLength(
          Crypto::cryptoAlgoBlockSize(Crypto::CryptoAlgo::AES));

  userMetadata->push_back(std::make_pair("encryptiondata", buildEncryptionMetadataJSON(ivEncoded, fileMetadata->encryptionMetadata.enKekEncoded) ));
  //userMetadata->push_back(std::make_pair("contentLength", std::to_string(fileMetadata->encryptionMetadata.cipherStreamSize)));

  //userMetadata->push_back(std::make_pair(SFC_DIGEST, fileMetadata->sha256Digest));

}

/*
RemoteStorageRequestOutcome SnowflakeAzureClient::handleError(
  const Aws::Client::AWSError<Aws::S3::S3Errors> & error)
{
  if (error.GetExceptionName() == "ExpiredToken")
  {
    CXX_LOG_WARN("Token expired.");
    return RemoteStorageRequestOutcome::TOKEN_EXPIRED;
  }
  else
  {
    CXX_LOG_ERROR("S3 request failed failed: %s",
                 error.GetMessage().c_str());
    return RemoteStorageRequestOutcome::FAILED;
  }
}
*/
RemoteStorageRequestOutcome SnowflakeAzureClient::download(
  FileMetadata *fileMetadata,
  std::basic_iostream<char>* dataStream)
{
  if (fileMetadata->srcFileSize > DATA_SIZE_THRESHOLD)
    return doMultiPartDownload(fileMetadata, dataStream);
  else
    return doSingleDownload(fileMetadata, dataStream);
}

RemoteStorageRequestOutcome SnowflakeAzureClient::doMultiPartDownload(
  FileMetadata *fileMetadata,
  std::basic_iostream<char> * dataStream)
{
  CXX_LOG_DEBUG("Start multi part download for file %s, parallel: %d",
               fileMetadata->srcFileName.c_str(), m_parallel);

  return RemoteStorageRequestOutcome::SUCCESS;
}

RemoteStorageRequestOutcome SnowflakeAzureClient::doSingleDownload(
  FileMetadata *fileMetadata,
  std::basic_iostream<char> * dataStream)
{
  CXX_LOG_DEBUG("Start single part download for file %s",
               fileMetadata->srcFileName.c_str());
  return RemoteStorageRequestOutcome::SUCCESS;
}

RemoteStorageRequestOutcome SnowflakeAzureClient::GetRemoteFileMetadata(
  std::string *filePathFull, FileMetadata *fileMetadata)
{
  return RemoteStorageRequestOutcome::SUCCESS;

}

}
}