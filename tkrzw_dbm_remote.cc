/*************************************************************************************************
 * Remote database manager implementation based on gRPC
 *
 * Copyright 2020 Google LLC
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
 * except in compliance with the License.  You may obtain a copy of the License at
 *     https://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software distributed under the
 * License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied.  See the License for the specific language governing permissions
 * and limitations under the License.
 *************************************************************************************************/

#include <grpc/grpc.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include "tkrzw_cmd_util.h"
#include "tkrzw_dbm_remote.h"
#include "tkrzw_rpc.grpc.pb.h"
#include "tkrzw_rpc.pb.h"

namespace tkrzw {

const char* GRPCStatusCodeName(grpc::StatusCode code) {
  switch (code) {
    case grpc::StatusCode::OK: return "OK";
    case grpc::StatusCode::CANCELLED: return "CANCELLED";
    case grpc::StatusCode::UNKNOWN: return "UNKNOWN";
    case grpc::StatusCode::INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case grpc::StatusCode::DEADLINE_EXCEEDED: return "DEADLINE_EXCEEDED";
    case grpc::StatusCode::NOT_FOUND: return "NOT_FOUND";
    case grpc::StatusCode::ALREADY_EXISTS: return "ALREADY_EXISTS";
    case grpc::StatusCode::PERMISSION_DENIED: return "PERMISSION_DENIED";
    case grpc::StatusCode::UNAUTHENTICATED: return "UNAUTHENTICATED";
    case grpc::StatusCode::RESOURCE_EXHAUSTED: return "RESOURCE_EXHAUSTED";
    case grpc::StatusCode::FAILED_PRECONDITION: return "FAILED_PRECONDITION";
    case grpc::StatusCode::ABORTED: return "ABORTED";
    case grpc::StatusCode::OUT_OF_RANGE: return "OUT_OF_RANGE";
    case grpc::StatusCode::UNIMPLEMENTED: return "UNIMPLEMENTED";
    case grpc::StatusCode::INTERNAL: return "INTERNAL";
    case grpc::StatusCode::UNAVAILABLE: return "UNAVAILABLE";
    case grpc::StatusCode::DATA_LOSS: return "DATA_LOSS";
    default: break;
  }
  return "unknown";
}

std::string GRPCStatusString(const grpc::Status& status) {
  const std::string& msg = status.error_message();
  if (msg.empty()) {
    return GRPCStatusCodeName(status.error_code());
  }
  return StrCat(GRPCStatusCodeName(status.error_code()), ": ", msg);
}

Status MakeStatusFromProto(const tkrzw::StatusProto& proto) {
  const auto& message = proto.message();
  if (message.empty()) {
    return Status(tkrzw::Status::Code(proto.code()));
  }
  return Status(tkrzw::Status::Code(proto.code()), message);
}

class RemoteDBMImpl final {
  friend class RemoteDBMStreamImpl;
  friend class RemoteDBMIteratorImpl;
  friend class RemoteDBMReplicatorImpl;
  typedef std::list<RemoteDBMStreamImpl*> StreamList;
  typedef std::list<RemoteDBMIteratorImpl*> IteratorList;
  typedef std::list<RemoteDBMReplicatorImpl*> ReplicatorList;
 public:
  RemoteDBMImpl();
  ~RemoteDBMImpl();
  void InjectStub(void* stub);
  Status Connect(const std::string& address, double timeout);
  Status Disconnect();
  Status SetDBMIndex(int32_t dbm_index);
  Status Echo(std::string_view message, std::string* echo);
  Status Inspect(std::vector<std::pair<std::string, std::string>>* records);
  Status Get(std::string_view key, std::string* value);
  Status GetMulti(
      const std::vector<std::string_view>& keys, std::map<std::string, std::string>* records);
  Status Set(std::string_view key, std::string_view value, bool overwrite);
  Status SetMulti(
      const std::map<std::string_view, std::string_view>& records, bool overwrite);
  Status Remove(std::string_view key);
  Status RemoveMulti(const std::vector<std::string_view>& keys);
  Status Append(std::string_view key, std::string_view value, std::string_view delim);
  Status AppendMulti(
      const std::map<std::string_view, std::string_view>& records, std::string_view delim);
  Status CompareExchange(std::string_view key, std::string_view expected,
                         std::string_view desired);
  Status Increment(std::string_view key, int64_t increment, int64_t* current, int64_t initial);
  Status CompareExchangeMulti(
      const std::vector<std::pair<std::string_view, std::string_view>>& expected,
      const std::vector<std::pair<std::string_view, std::string_view>>& desired);
  Status Count(int64_t* count);
  Status GetFileSize(int64_t* file_size);
  Status Clear();
  Status Rebuild(const std::map<std::string, std::string>& params);
  Status ShouldBeRebuilt(bool* tobe);
  Status Synchronize(bool hard, const std::map<std::string, std::string>& params);
  Status SearchModal(std::string_view mode, std::string_view pattern,
                     std::vector<std::string>* matched, size_t capacity);
  Status ChangeMaster(std::string_view master, double timestamp_skew);

 private:
  std::unique_ptr<DBMService::StubInterface> stub_;
  double timeout_;
  int32_t dbm_index_;
  StreamList streams_;
  IteratorList iterators_;
  ReplicatorList replicators_;
  SpinSharedMutex mutex_;
};

class RemoteDBMStreamImpl final {
  friend class RemoteDBMImpl;
 public:
  explicit RemoteDBMStreamImpl(RemoteDBMImpl* dbm);
  ~RemoteDBMStreamImpl();
  void Cancel();
  Status Echo(std::string_view message, std::string* echo);
  Status Get(std::string_view key, std::string* value);
  Status Set(std::string_view key, std::string_view value, bool overwrite, bool ignore_result);
  Status Remove(std::string_view key, bool ignore_result);
  Status Append(std::string_view key, std::string_view value, std::string_view delim,
                bool ignore_result);
  Status CompareExchange(
      std::string_view key, std::string_view expected, std::string_view desired);
  Status Increment(std::string_view key, int64_t increment,
                   int64_t* current, int64_t initial, bool ignore_result);

 private:
  RemoteDBMImpl* dbm_;
  grpc::ClientContext context_;
  std::unique_ptr<grpc::ClientReaderWriterInterface<
                    tkrzw::StreamRequest, tkrzw::StreamResponse>> stream_;
  std::atomic_bool healthy_;
};

class RemoteDBMIteratorImpl final {
  friend class RemoteDBMImpl;
 public:
  explicit RemoteDBMIteratorImpl(RemoteDBMImpl* dbm);
  ~RemoteDBMIteratorImpl();
  void Cancel();
  Status First();
  Status Last();
  Status Jump(std::string_view key);
  Status JumpLower(std::string_view key, bool inclusive);
  Status JumpUpper(std::string_view key, bool inclusive);
  Status Next();
  Status Previous();
  Status Get(std::string* key, std::string* value);
  Status Set(std::string_view value);
  Status Remove();

 private:
  RemoteDBMImpl* dbm_;
  grpc::ClientContext context_;
  std::unique_ptr<grpc::ClientReaderWriterInterface<
                    tkrzw::IterateRequest, tkrzw::IterateResponse>> stream_;
  std::atomic_bool healthy_;
};

class RemoteDBMReplicatorImpl final {
  friend class RemoteDBMImpl;
 public:
  explicit RemoteDBMReplicatorImpl(RemoteDBMImpl* dbm);
  ~RemoteDBMReplicatorImpl();
  void Cancel();
  int32_t GetMasterServerID();
  Status Start(int64_t min_timestamp, int32_t server_id, double wait_time);
  Status Read(int64_t* timestamp, RemoteDBM::ReplicateLog* op);

 private:
  RemoteDBMImpl* dbm_;
  grpc::ClientContext context_;
  std::unique_ptr<grpc::ClientReaderInterface<tkrzw::ReplicateResponse>> stream_;
  std::atomic_bool healthy_;
  int32_t server_id_;
};

RemoteDBMImpl::RemoteDBMImpl()
    : stub_(nullptr), timeout_(0), dbm_index_(0),
      streams_(), iterators_(), replicators_(), mutex_() {}

RemoteDBMImpl::~RemoteDBMImpl() {
  for (auto* stream : streams_) {
    stream->dbm_ = nullptr;
  }
  for (auto* iterator : iterators_) {
    iterator->dbm_ = nullptr;
  }
  for (auto* replicator : replicators_) {
    replicator->dbm_ = nullptr;
  }
}

void RemoteDBMImpl::InjectStub(void* stub) {
  stub_.reset(reinterpret_cast<DBMService::StubInterface*>(stub));
}

Status RemoteDBMImpl::Connect(const std::string& address, double timeout) {
  std::lock_guard<SpinSharedMutex> lock(mutex_);
  if (stub_ != nullptr) {
    return Status(Status::PRECONDITION_ERROR, "connected database");
  }
  if (timeout < 0) {
    timeout = INT32MAX;
  }
  auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
  const auto deadline = std::chrono::system_clock::now() +
      std::chrono::microseconds(static_cast<int64_t>(timeout * 1000000));
  while (true) {
    auto status = channel->GetState(true);
    if (status == GRPC_CHANNEL_READY) {
      break;
    }
    if ((status != GRPC_CHANNEL_IDLE && status != GRPC_CHANNEL_CONNECTING) ||
        !channel->WaitForStateChange(status, deadline)) {
      return Status(Status::NETWORK_ERROR, "connection failed");
    }
  }
  stub_ = DBMService::NewStub(channel);
  timeout_ = timeout;
  return Status(Status::SUCCESS);
}

Status RemoteDBMImpl::Disconnect() {
  std::lock_guard<SpinSharedMutex> lock(mutex_);
  if (stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  stub_.reset(nullptr);
  return Status(Status::SUCCESS);
}

Status RemoteDBMImpl::SetDBMIndex(int32_t dbm_index) {
  std::lock_guard<SpinSharedMutex> lock(mutex_);
  if (stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  dbm_index_ = dbm_index;
  return Status(Status::SUCCESS);
}

Status RemoteDBMImpl::Echo(std::string_view message, std::string* echo) {
  std::shared_lock<SpinSharedMutex> lock(mutex_);
  if (stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::microseconds(static_cast<int64_t>(timeout_ * 1000000)));
  EchoRequest request;
  request.set_message(std::string(message));
  EchoResponse response;
  grpc::Status status = stub_->Echo(&context, request, &response);
  if (!status.ok()) {
    return Status(Status::NETWORK_ERROR, GRPCStatusString(status));
  }
  *echo = response.echo();
  return Status(Status::SUCCESS);
}

Status RemoteDBMImpl::Inspect(std::vector<std::pair<std::string, std::string>>* records) {
  std::shared_lock<SpinSharedMutex> lock(mutex_);
  if (stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::microseconds(static_cast<int64_t>(timeout_ * 1000000)));
  InspectRequest request;
  request.set_dbm_index(dbm_index_);
  InspectResponse response;
  grpc::Status status = stub_->Inspect(&context, request, &response);
  if (!status.ok()) {
    return Status(Status::NETWORK_ERROR, GRPCStatusString(status));
  }
  for (const auto& record : response.records()) {
    records->emplace_back(std::make_pair(record.first(), record.second()));
  }
  return Status(Status::SUCCESS);
}

Status RemoteDBMImpl::Get(std::string_view key, std::string* value) {
  std::shared_lock<SpinSharedMutex> lock(mutex_);
  if (stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::microseconds(static_cast<int64_t>(timeout_ * 1000000)));
  GetRequest request;
  request.set_dbm_index(dbm_index_);
  request.set_key(key.data(), key.size());
  if (value == nullptr) {
    request.set_omit_value(true);
  }
  GetResponse response;
  grpc::Status status = stub_->Get(&context, request, &response);
  if (!status.ok()) {
    return Status(Status::NETWORK_ERROR, GRPCStatusString(status));
  }
  if (response.status().code() == 0 && value != nullptr) {
    *value = response.value();
  }
  return MakeStatusFromProto(response.status());
}

Status RemoteDBMImpl::GetMulti(
    const std::vector<std::string_view>& keys, std::map<std::string, std::string>* records) {
  std::shared_lock<SpinSharedMutex> lock(mutex_);
  if (stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::microseconds(static_cast<int64_t>(timeout_ * 1000000)));
  GetMultiRequest request;
  request.set_dbm_index(dbm_index_);
  for (const auto& key : keys) {
    request.add_keys(std::string(key));
  }
  GetMultiResponse response;
  grpc::Status status = stub_->GetMulti(&context, request, &response);
  if (!status.ok()) {
    return Status(Status::NETWORK_ERROR, GRPCStatusString(status));
  }
  for (const auto& record : response.records()) {
    records->emplace(std::make_pair(record.first(), record.second()));
  }
  return MakeStatusFromProto(response.status());
}

Status RemoteDBMImpl::Set(std::string_view key, std::string_view value, bool overwrite) {
  std::shared_lock<SpinSharedMutex> lock(mutex_);
  if (stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::microseconds(static_cast<int64_t>(timeout_ * 1000000)));
  SetRequest request;
  request.set_dbm_index(dbm_index_);
  request.set_key(key.data(), key.size());
  request.set_value(value.data(), value.size());
  request.set_overwrite(overwrite);
  SetResponse response;
  grpc::Status status = stub_->Set(&context, request, &response);
  if (!status.ok()) {
    return Status(Status::NETWORK_ERROR, GRPCStatusString(status));
  }
  return MakeStatusFromProto(response.status());
}

Status RemoteDBMImpl::SetMulti(
    const std::map<std::string_view, std::string_view>& records, bool overwrite) {
  std::shared_lock<SpinSharedMutex> lock(mutex_);
  if (stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::microseconds(static_cast<int64_t>(timeout_ * 1000000)));
  SetMultiRequest request;
  request.set_dbm_index(dbm_index_);
  for (const auto& record : records) {
    auto* req_record = request.add_records();
    req_record->set_first(std::string(record.first));
    req_record->set_second(std::string(record.second));
  }
  request.set_overwrite(overwrite);
  SetMultiResponse response;
  grpc::Status status = stub_->SetMulti(&context, request, &response);
  if (!status.ok()) {
    return Status(Status::NETWORK_ERROR, GRPCStatusString(status));
  }
  return MakeStatusFromProto(response.status());
}

Status RemoteDBMImpl::Remove(std::string_view key) {
  std::shared_lock<SpinSharedMutex> lock(mutex_);
  if (stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::microseconds(static_cast<int64_t>(timeout_ * 1000000)));
  RemoveRequest request;
  request.set_dbm_index(dbm_index_);
  request.set_key(key.data(), key.size());
  RemoveResponse response;
  grpc::Status status = stub_->Remove(&context, request, &response);
  if (!status.ok()) {
    return Status(Status::NETWORK_ERROR, GRPCStatusString(status));
  }
  return MakeStatusFromProto(response.status());
}

Status RemoteDBMImpl::RemoveMulti(const std::vector<std::string_view>& keys) {
  std::shared_lock<SpinSharedMutex> lock(mutex_);
  if (stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::microseconds(static_cast<int64_t>(timeout_ * 1000000)));
  RemoveMultiRequest request;
  request.set_dbm_index(dbm_index_);
  for (const auto& key : keys) {
    request.add_keys(std::string(key));
  }
  RemoveMultiResponse response;
  grpc::Status status = stub_->RemoveMulti(&context, request, &response);
  if (!status.ok()) {
    return Status(Status::NETWORK_ERROR, GRPCStatusString(status));
  }
  return MakeStatusFromProto(response.status());
}

Status RemoteDBMImpl::Append(
    std::string_view key, std::string_view value, std::string_view delim) {
  std::shared_lock<SpinSharedMutex> lock(mutex_);
  if (stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::microseconds(static_cast<int64_t>(timeout_ * 1000000)));
  AppendRequest request;
  request.set_dbm_index(dbm_index_);
  request.set_key(key.data(), key.size());
  request.set_value(value.data(), value.size());
  request.set_delim(delim.data(), delim.size());
  AppendResponse response;
  grpc::Status status = stub_->Append(&context, request, &response);
  if (!status.ok()) {
    return Status(Status::NETWORK_ERROR, GRPCStatusString(status));
  }
  return MakeStatusFromProto(response.status());
}

Status RemoteDBMImpl::AppendMulti(
    const std::map<std::string_view, std::string_view>& records, std::string_view delim) {
  std::shared_lock<SpinSharedMutex> lock(mutex_);
  if (stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::microseconds(static_cast<int64_t>(timeout_ * 1000000)));
  AppendMultiRequest request;
  request.set_dbm_index(dbm_index_);
  for (const auto& record : records) {
    auto* req_record = request.add_records();
    req_record->set_first(std::string(record.first));
    req_record->set_second(std::string(record.second));
  }
  request.set_delim(std::string(delim));
  AppendMultiResponse response;
  grpc::Status status = stub_->AppendMulti(&context, request, &response);
  if (!status.ok()) {
    return Status(Status::NETWORK_ERROR, GRPCStatusString(status));
  }
  return MakeStatusFromProto(response.status());
}

Status RemoteDBMImpl::CompareExchange(std::string_view key, std::string_view expected,
                                      std::string_view desired) {
  std::shared_lock<SpinSharedMutex> lock(mutex_);
  if (stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::microseconds(static_cast<int64_t>(timeout_ * 1000000)));
  CompareExchangeRequest request;
  request.set_dbm_index(dbm_index_);
  request.set_key(key.data(), key.size());
  if (expected.data() != nullptr) {
    request.set_expected_existence(true);
    request.set_expected_value(expected.data(), expected.size());
  }
  if (desired.data() != nullptr) {
    request.set_desired_existence(true);
    request.set_desired_value(desired.data(), desired.size());
  }
  CompareExchangeResponse response;
  grpc::Status status = stub_->CompareExchange(&context, request, &response);
  if (!status.ok()) {
    return Status(Status::NETWORK_ERROR, GRPCStatusString(status));
  }
  return MakeStatusFromProto(response.status());
}

Status RemoteDBMImpl::Increment(
    std::string_view key, int64_t increment, int64_t* current, int64_t initial) {
  std::shared_lock<SpinSharedMutex> lock(mutex_);
  if (stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::microseconds(static_cast<int64_t>(timeout_ * 1000000)));
  IncrementRequest request;
  request.set_dbm_index(dbm_index_);
  request.set_key(key.data(), key.size());
  request.set_increment(increment);
  request.set_initial(initial);
  IncrementResponse response;
  grpc::Status status = stub_->Increment(&context, request, &response);
  if (!status.ok()) {
    return Status(Status::NETWORK_ERROR, GRPCStatusString(status));
  }
  if (current != nullptr) {
    *current = response.current();
  }
  return MakeStatusFromProto(response.status());
}

Status RemoteDBMImpl::CompareExchangeMulti(
    const std::vector<std::pair<std::string_view, std::string_view>>& expected,
    const std::vector<std::pair<std::string_view, std::string_view>>& desired) {
  std::shared_lock<SpinSharedMutex> lock(mutex_);
  if (stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::microseconds(static_cast<int64_t>(timeout_ * 1000000)));
  CompareExchangeMultiRequest request;
  request.set_dbm_index(dbm_index_);
  for (const auto& record : expected) {
    auto* req_record = request.add_expected();
    req_record->set_key(std::string(record.first));
    if (record.second.data() != nullptr) {
      req_record->set_existence(true);
      req_record->set_value(std::string(record.second));
    }
  }
  for (const auto& record : desired) {
    auto* req_record = request.add_desired();
    req_record->set_key(std::string(record.first));
    if (record.second.data() != nullptr) {
      req_record->set_existence(true);
      req_record->set_value(std::string(record.second));
    }
  }
  CompareExchangeMultiResponse response;
  grpc::Status status = stub_->CompareExchangeMulti(&context, request, &response);
  if (!status.ok()) {
    return Status(Status::NETWORK_ERROR, GRPCStatusString(status));
  }
  return MakeStatusFromProto(response.status());
}

Status RemoteDBMImpl::Count(int64_t* count) {
  std::shared_lock<SpinSharedMutex> lock(mutex_);
  if (stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::microseconds(static_cast<int64_t>(timeout_ * 1000000)));
  CountRequest request;
  CountResponse response;
  grpc::Status status = stub_->Count(&context, request, &response);
  if (!status.ok()) {
    return Status(Status::NETWORK_ERROR, GRPCStatusString(status));
  }
  if (response.status().code() == 0) {
    *count = response.count();
  }
  return MakeStatusFromProto(response.status());
}

Status RemoteDBMImpl::GetFileSize(int64_t* file_size) {
  std::shared_lock<SpinSharedMutex> lock(mutex_);
  if (stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::microseconds(static_cast<int64_t>(timeout_ * 1000000)));
  GetFileSizeRequest request;
  GetFileSizeResponse response;
  grpc::Status status = stub_->GetFileSize(&context, request, &response);
  if (!status.ok()) {
    return Status(Status::NETWORK_ERROR, GRPCStatusString(status));
  }
  if (response.status().code() == 0) {
    *file_size = response.file_size();
  }
  return MakeStatusFromProto(response.status());
}

Status RemoteDBMImpl::Clear() {
  std::shared_lock<SpinSharedMutex> lock(mutex_);
  if (stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::microseconds(static_cast<int64_t>(timeout_ * 1000000)));
  ClearRequest request;
  ClearResponse response;
  grpc::Status status = stub_->Clear(&context, request, &response);
  if (!status.ok()) {
    return Status(Status::NETWORK_ERROR, GRPCStatusString(status));
  }
  return MakeStatusFromProto(response.status());
}

Status RemoteDBMImpl::Rebuild(const std::map<std::string, std::string>& params) {
  std::shared_lock<SpinSharedMutex> lock(mutex_);
  if (stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::microseconds(static_cast<int64_t>(timeout_ * 1000000)));
  RebuildRequest request;
  for (const auto& param : params) {
    auto* req_param = request.add_params();
    req_param->set_first(param.first);
    req_param->set_second(param.second);
  }
  RebuildResponse response;
  grpc::Status status = stub_->Rebuild(&context, request, &response);
  if (!status.ok()) {
    return Status(Status::NETWORK_ERROR, GRPCStatusString(status));
  }
  return MakeStatusFromProto(response.status());
}

Status RemoteDBMImpl::ShouldBeRebuilt(bool* tobe) {
  std::shared_lock<SpinSharedMutex> lock(mutex_);
  if (stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::microseconds(static_cast<int64_t>(timeout_ * 1000000)));
  ShouldBeRebuiltRequest request;
  ShouldBeRebuiltResponse response;
  grpc::Status status = stub_->ShouldBeRebuilt(&context, request, &response);
  if (!status.ok()) {
    return Status(Status::NETWORK_ERROR, GRPCStatusString(status));
  }
  if (response.status().code() == 0) {
    *tobe = response.tobe();
  }
  return MakeStatusFromProto(response.status());
}

Status RemoteDBMImpl::Synchronize(bool hard, const std::map<std::string, std::string>& params) {
  std::shared_lock<SpinSharedMutex> lock(mutex_);
  if (stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::microseconds(static_cast<int64_t>(timeout_ * 1000000)));
  SynchronizeRequest request;
  request.set_hard(hard);
  for (const auto& param : params) {
    auto* req_param = request.add_params();
    req_param->set_first(param.first);
    req_param->set_second(param.second);
  }
  SynchronizeResponse response;
  grpc::Status status = stub_->Synchronize(&context, request, &response);
  if (!status.ok()) {
    return Status(Status::NETWORK_ERROR, GRPCStatusString(status));
  }
  return MakeStatusFromProto(response.status());
}

Status RemoteDBMImpl::SearchModal(std::string_view mode, std::string_view pattern,
                                  std::vector<std::string>* matched, size_t capacity) {
  std::shared_lock<SpinSharedMutex> lock(mutex_);
  if (stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::microseconds(static_cast<int64_t>(timeout_ * 1000000)));
  SearchModalRequest request;
  request.set_mode(std::string(mode));
  request.set_pattern(pattern.data(), pattern.size());
  request.set_capacity(capacity);
  SearchModalResponse response;
  grpc::Status status = stub_->SearchModal(&context, request, &response);
  if (!status.ok()) {
    return Status(Status::NETWORK_ERROR, GRPCStatusString(status));
  }
  if (response.status().code() == 0) {
    matched->reserve(response.matched_size());
    matched->insert(matched->end(), response.matched().begin(), response.matched().end());
  }
  return MakeStatusFromProto(response.status());
}

Status RemoteDBMImpl::ChangeMaster(std::string_view master, double timestamp_skew) {
  std::shared_lock<SpinSharedMutex> lock(mutex_);
  if (stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::microseconds(static_cast<int64_t>(timeout_ * 1000000)));
  ChangeMasterRequest request;
  request.set_master(std::string(master));
  request.set_timestamp_skew(timestamp_skew);
  ChangeMasterResponse response;
  grpc::Status status = stub_->ChangeMaster(&context, request, &response);
  if (!status.ok()) {
    return Status(Status::NETWORK_ERROR, GRPCStatusString(status));
  }
  return MakeStatusFromProto(response.status());
}

RemoteDBMStreamImpl::RemoteDBMStreamImpl(RemoteDBMImpl* dbm)
    : dbm_(dbm), context_(), stream_(nullptr), healthy_(true) {
  {
    std::lock_guard<SpinSharedMutex> lock(dbm_->mutex_);
    dbm_->streams_.emplace_back(this);
  }
  std::shared_lock<SpinSharedMutex> lock(dbm_->mutex_);
  context_.set_deadline(std::chrono::system_clock::now() + std::chrono::microseconds(
      static_cast<int64_t>(dbm_->timeout_ * 1000000)));
  stream_ = dbm_->stub_->Stream(&context_);
}

RemoteDBMStreamImpl::~RemoteDBMStreamImpl() {
  if (dbm_ != nullptr) {
    if (healthy_.load()) {
      std::shared_lock<SpinSharedMutex> lock(dbm_->mutex_);
      stream_->WritesDone();
      stream_->Finish();
    }
    std::lock_guard<SpinSharedMutex> lock(dbm_->mutex_);
    dbm_->streams_.remove(this);
  }
}

void RemoteDBMStreamImpl::Cancel() {
  healthy_.store(false);
  context_.TryCancel();
}

Status RemoteDBMStreamImpl::Echo(std::string_view message, std::string* echo) {
  std::shared_lock<SpinSharedMutex> lock(dbm_->mutex_);
  if (dbm_->stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  if (!healthy_.load()) {
    return Status(Status::PRECONDITION_ERROR, "unhealthy stream");
  }
  context_.set_deadline(std::chrono::system_clock::now() + std::chrono::microseconds(
      static_cast<int64_t>(dbm_->timeout_ * 1000000)));
  StreamRequest stream_request;
  auto* request = stream_request.mutable_echo_request();
  request->set_message(std::string(message));
  if (!stream_->Write(stream_request)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Write failed: ", message));
  }
  StreamResponse stream_response;
  if (!stream_->Read(&stream_response)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Read failed: ", message));
  }
  const EchoResponse& response = stream_response.echo_response();
  *echo = response.echo();
  return Status(Status::SUCCESS);
}

Status RemoteDBMStreamImpl::Get(std::string_view key, std::string* value) {
  std::shared_lock<SpinSharedMutex> lock(dbm_->mutex_);
  if (dbm_->stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  if (!healthy_.load()) {
    return Status(Status::PRECONDITION_ERROR, "unhealthy stream");
  }
  context_.set_deadline(std::chrono::system_clock::now() + std::chrono::microseconds(
      static_cast<int64_t>(dbm_->timeout_ * 1000000)));
  StreamRequest stream_request;
  auto* request = stream_request.mutable_get_request();
  request->set_dbm_index(dbm_->dbm_index_);
  request->set_key(key.data(), key.size());
  if (value == nullptr) {
    request->set_omit_value(true);
  }
  if (!stream_->Write(stream_request)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Write failed: ", message));
  }
  StreamResponse stream_response;
  if (!stream_->Read(&stream_response)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Read failed: ", message));
  }
  const GetResponse& response = stream_response.get_response();
  if (response.status().code() == 0) {
    if (value != nullptr) {
      *value = response.value();
    }
  }
  return MakeStatusFromProto(response.status());
}

Status RemoteDBMStreamImpl::Set(std::string_view key, std::string_view value,
                                bool overwrite, bool ignore_result) {
  std::shared_lock<SpinSharedMutex> lock(dbm_->mutex_);
  if (dbm_->stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  if (!healthy_.load()) {
    return Status(Status::PRECONDITION_ERROR, "unhealthy stream");
  }
  context_.set_deadline(std::chrono::system_clock::now() + std::chrono::microseconds(
      static_cast<int64_t>(dbm_->timeout_ * 1000000)));
  StreamRequest stream_request;
  auto* request = stream_request.mutable_set_request();
  request->set_dbm_index(dbm_->dbm_index_);
  request->set_key(key.data(), key.size());
  request->set_value(value.data(), value.size());
  request->set_overwrite(overwrite);
  if (ignore_result) {
    stream_request.set_omit_response(true);
  }
  if (!stream_->Write(stream_request)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Write failed: ", message));
  }
  if (ignore_result) {
    return Status(Status::SUCCESS);
  }
  StreamResponse stream_response;
  if (!stream_->Read(&stream_response)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Read failed: ", message));
  }
  const SetResponse& response = stream_response.set_response();
  return MakeStatusFromProto(response.status());
}

Status RemoteDBMStreamImpl::Remove(std::string_view key, bool ignore_result) {
  std::shared_lock<SpinSharedMutex> lock(dbm_->mutex_);
  if (dbm_->stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  if (!healthy_.load()) {
    return Status(Status::PRECONDITION_ERROR, "unhealthy stream");
  }
  context_.set_deadline(std::chrono::system_clock::now() + std::chrono::microseconds(
      static_cast<int64_t>(dbm_->timeout_ * 1000000)));
  StreamRequest stream_request;
  auto* request = stream_request.mutable_remove_request();
  request->set_dbm_index(dbm_->dbm_index_);
  request->set_key(key.data(), key.size());
  if (ignore_result) {
    stream_request.set_omit_response(true);
  }
  if (!stream_->Write(stream_request)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Write failed: ", message));
  }
  if (ignore_result) {
    return Status(Status::SUCCESS);
  }
  StreamResponse stream_response;
  if (!stream_->Read(&stream_response)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Read failed: ", message));
  }
  const RemoveResponse& response = stream_response.remove_response();
  return MakeStatusFromProto(response.status());
}

Status RemoteDBMStreamImpl::Append(
    std::string_view key, std::string_view value, std::string_view delim, bool ignore_result) {
  std::shared_lock<SpinSharedMutex> lock(dbm_->mutex_);
  if (dbm_->stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  if (!healthy_.load()) {
    return Status(Status::PRECONDITION_ERROR, "unhealthy stream");
  }
  context_.set_deadline(std::chrono::system_clock::now() + std::chrono::microseconds(
      static_cast<int64_t>(dbm_->timeout_ * 1000000)));
  StreamRequest stream_request;
  auto* request = stream_request.mutable_append_request();
  request->set_dbm_index(dbm_->dbm_index_);
  request->set_key(key.data(), key.size());
  request->set_value(value.data(), value.size());
  request->set_delim(delim.data(), delim.size());
  if (ignore_result) {
    stream_request.set_omit_response(true);
  }
  if (!stream_->Write(stream_request)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Write failed: ", message));
  }
  if (ignore_result) {
    return Status(Status::SUCCESS);
  }
  StreamResponse stream_response;
  if (!stream_->Read(&stream_response)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Read failed: ", message));
  }
  const AppendResponse& response = stream_response.append_response();
  return MakeStatusFromProto(response.status());
}

Status RemoteDBMStreamImpl::CompareExchange(
    std::string_view key, std::string_view expected, std::string_view desired) {
  std::shared_lock<SpinSharedMutex> lock(dbm_->mutex_);
  if (dbm_->stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  if (!healthy_.load()) {
    return Status(Status::PRECONDITION_ERROR, "unhealthy stream");
  }
  context_.set_deadline(std::chrono::system_clock::now() + std::chrono::microseconds(
      static_cast<int64_t>(dbm_->timeout_ * 1000000)));
  StreamRequest stream_request;
  auto* request = stream_request.mutable_compare_exchange_request();
  request->set_dbm_index(dbm_->dbm_index_);
  request->set_key(key.data(), key.size());
  if (expected.data() != nullptr) {
    request->set_expected_existence(true);
    request->set_expected_value(expected.data(), expected.size());
  }
  if (desired.data() != nullptr) {
    request->set_desired_existence(true);
    request->set_desired_value(desired.data(), desired.size());
  }
  if (!stream_->Write(stream_request)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Write failed: ", message));
  }
  StreamResponse stream_response;
  if (!stream_->Read(&stream_response)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Read failed: ", message));
  }
  const CompareExchangeResponse& response = stream_response.compare_exchange_response();
  return MakeStatusFromProto(response.status());
}

Status RemoteDBMStreamImpl::Increment(
    std::string_view key, int64_t increment,
    int64_t* current, int64_t initial, bool ignore_result) {
  std::shared_lock<SpinSharedMutex> lock(dbm_->mutex_);
  if (dbm_->stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  if (!healthy_.load()) {
    return Status(Status::PRECONDITION_ERROR, "unhealthy stream");
  }
  context_.set_deadline(std::chrono::system_clock::now() + std::chrono::microseconds(
      static_cast<int64_t>(dbm_->timeout_ * 1000000)));
  StreamRequest stream_request;
  auto* request = stream_request.mutable_increment_request();
  request->set_dbm_index(dbm_->dbm_index_);
  request->set_key(key.data(), key.size());
  request->set_increment(increment);
  request->set_initial(initial);
  if (!stream_->Write(stream_request)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Write failed: ", message));
  }
  if (ignore_result) {
    return Status(Status::SUCCESS);
  }
  StreamResponse stream_response;
  if (!stream_->Read(&stream_response)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Read failed: ", message));
  }
  const IncrementResponse& response = stream_response.increment_response();
  if (current != nullptr) {
    *current = response.current();
  }
  return MakeStatusFromProto(response.status());
}

RemoteDBMIteratorImpl::RemoteDBMIteratorImpl(RemoteDBMImpl* dbm)
    : dbm_(dbm), context_(), stream_(nullptr), healthy_(true) {
  {
    std::lock_guard<SpinSharedMutex> lock(dbm_->mutex_);
    dbm_->iterators_.emplace_back(this);
  }
  std::shared_lock<SpinSharedMutex> lock(dbm_->mutex_);
  context_.set_deadline(std::chrono::system_clock::now() + std::chrono::microseconds(
      static_cast<int64_t>(dbm_->timeout_ * 1000000)));
  stream_ = dbm_->stub_->Iterate(&context_);
}

RemoteDBMIteratorImpl::~RemoteDBMIteratorImpl() {
  if (dbm_ != nullptr) {
    if (healthy_.load()) {
      std::shared_lock<SpinSharedMutex> lock(dbm_->mutex_);
      stream_->WritesDone();
      stream_->Finish();
    }
    std::lock_guard<SpinSharedMutex> lock(dbm_->mutex_);
    dbm_->iterators_.remove(this);
  }
}

void RemoteDBMIteratorImpl::Cancel() {
  healthy_.store(false);
  context_.TryCancel();
}

Status RemoteDBMIteratorImpl::First() {
  std::shared_lock<SpinSharedMutex> lock(dbm_->mutex_);
  if (dbm_->stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  if (!healthy_.load()) {
    return Status(Status::PRECONDITION_ERROR, "unhealthy stream");
  }
  context_.set_deadline(std::chrono::system_clock::now() + std::chrono::microseconds(
      static_cast<int64_t>(dbm_->timeout_ * 1000000)));
  IterateRequest request;
  request.set_dbm_index(dbm_->dbm_index_);
  request.set_operation(IterateRequest::OP_FIRST);
  if (!stream_->Write(request)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Write failed: ", message));
  }
  IterateResponse response;
  if (!stream_->Read(&response)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Read failed: ", message));
  }
  return MakeStatusFromProto(response.status());
}

Status RemoteDBMIteratorImpl::Last() {
  std::shared_lock<SpinSharedMutex> lock(dbm_->mutex_);
  if (dbm_->stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  if (!healthy_.load()) {
    return Status(Status::PRECONDITION_ERROR, "unhealthy stream");
  }
  context_.set_deadline(std::chrono::system_clock::now() + std::chrono::microseconds(
      static_cast<int64_t>(dbm_->timeout_ * 1000000)));
  IterateRequest request;
  request.set_dbm_index(dbm_->dbm_index_);
  request.set_operation(IterateRequest::OP_LAST);
  if (!stream_->Write(request)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Write failed: ", message));
  }
  IterateResponse response;
  if (!stream_->Read(&response)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Read failed: ", message));
  }
  return MakeStatusFromProto(response.status());
}

Status RemoteDBMIteratorImpl::Jump(std::string_view key) {
  std::shared_lock<SpinSharedMutex> lock(dbm_->mutex_);
  if (dbm_->stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  if (!healthy_.load()) {
    return Status(Status::PRECONDITION_ERROR, "unhealthy stream");
  }
  context_.set_deadline(std::chrono::system_clock::now() + std::chrono::microseconds(
      static_cast<int64_t>(dbm_->timeout_ * 1000000)));
  IterateRequest request;
  request.set_dbm_index(dbm_->dbm_index_);
  request.set_operation(IterateRequest::OP_JUMP);
  request.set_key(std::string(key));
  if (!stream_->Write(request)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Write failed: ", message));
  }
  IterateResponse response;
  if (!stream_->Read(&response)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Read failed: ", message));
  }
  return MakeStatusFromProto(response.status());
}

Status RemoteDBMIteratorImpl::JumpLower(std::string_view key, bool inclusive) {
  if (dbm_->stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  if (!healthy_.load()) {
    return Status(Status::PRECONDITION_ERROR, "unhealthy stream");
  }
  context_.set_deadline(std::chrono::system_clock::now() + std::chrono::microseconds(
      static_cast<int64_t>(dbm_->timeout_ * 1000000)));
  IterateRequest request;
  request.set_dbm_index(dbm_->dbm_index_);
  request.set_operation(IterateRequest::OP_JUMP_LOWER);
  request.set_key(std::string(key));
  request.set_jump_inclusive(inclusive);
  if (!stream_->Write(request)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Write failed: ", message));
  }
  IterateResponse response;
  if (!stream_->Read(&response)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Read failed: ", message));
  }
  return MakeStatusFromProto(response.status());
}

Status RemoteDBMIteratorImpl::JumpUpper(std::string_view key, bool inclusive) {
  if (dbm_->stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  if (!healthy_.load()) {
    return Status(Status::PRECONDITION_ERROR, "unhealthy stream");
  }
  context_.set_deadline(std::chrono::system_clock::now() + std::chrono::microseconds(
      static_cast<int64_t>(dbm_->timeout_ * 1000000)));
  IterateRequest request;
  request.set_dbm_index(dbm_->dbm_index_);
  request.set_operation(IterateRequest::OP_JUMP_UPPER);
  request.set_key(std::string(key));
  request.set_jump_inclusive(inclusive);
  if (!stream_->Write(request)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Write failed: ", message));
  }
  IterateResponse response;
  if (!stream_->Read(&response)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Read failed: ", message));
  }
  return MakeStatusFromProto(response.status());
}

Status RemoteDBMIteratorImpl::Next() {
  std::shared_lock<SpinSharedMutex> lock(dbm_->mutex_);
  if (dbm_->stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  if (!healthy_.load()) {
    return Status(Status::PRECONDITION_ERROR, "unhealthy stream");
  }
  context_.set_deadline(std::chrono::system_clock::now() + std::chrono::microseconds(
      static_cast<int64_t>(dbm_->timeout_ * 1000000)));
  IterateRequest request;
  request.set_dbm_index(dbm_->dbm_index_);
  request.set_operation(IterateRequest::OP_NEXT);
  if (!stream_->Write(request)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Write failed: ", message));
  }
  IterateResponse response;
  if (!stream_->Read(&response)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Read failed: ", message));
  }
  return MakeStatusFromProto(response.status());
}

Status RemoteDBMIteratorImpl::Previous() {
  std::shared_lock<SpinSharedMutex> lock(dbm_->mutex_);
  if (dbm_->stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  if (!healthy_.load()) {
    return Status(Status::PRECONDITION_ERROR, "unhealthy stream");
  }
  context_.set_deadline(std::chrono::system_clock::now() + std::chrono::microseconds(
      static_cast<int64_t>(dbm_->timeout_ * 1000000)));
  IterateRequest request;
  request.set_dbm_index(dbm_->dbm_index_);
  request.set_operation(IterateRequest::OP_PREVIOUS);
  if (!stream_->Write(request)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Write failed: ", message));
  }
  IterateResponse response;
  if (!stream_->Read(&response)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Read failed: ", message));
  }
  return MakeStatusFromProto(response.status());
}

Status RemoteDBMIteratorImpl::Get(std::string* key, std::string* value) {
  std::shared_lock<SpinSharedMutex> lock(dbm_->mutex_);
  if (dbm_->stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  if (!healthy_.load()) {
    return Status(Status::PRECONDITION_ERROR, "unhealthy stream");
  }
  context_.set_deadline(std::chrono::system_clock::now() + std::chrono::microseconds(
      static_cast<int64_t>(dbm_->timeout_ * 1000000)));
  IterateRequest request;
  request.set_dbm_index(dbm_->dbm_index_);
  request.set_operation(IterateRequest::OP_GET);
  if (key == nullptr) {
    request.set_omit_key(true);
  }
  if (value == nullptr) {
    request.set_omit_value(true);
  }
  if (!stream_->Write(request)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Write failed: ", message));
  }
  IterateResponse response;
  if (!stream_->Read(&response)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Read failed: ", message));
  }
  if (response.status().code() == 0) {
    if (key != nullptr) {
      *key = response.key();
    }
    if (value != nullptr) {
      *value = response.value();
    }
  }
  return MakeStatusFromProto(response.status());
}

Status RemoteDBMIteratorImpl::Set(std::string_view value) {
  std::shared_lock<SpinSharedMutex> lock(dbm_->mutex_);
  if (dbm_->stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  if (!healthy_.load()) {
    return Status(Status::PRECONDITION_ERROR, "unhealthy stream");
  }
  context_.set_deadline(std::chrono::system_clock::now() + std::chrono::microseconds(
      static_cast<int64_t>(dbm_->timeout_ * 1000000)));
  IterateRequest request;
  request.set_dbm_index(dbm_->dbm_index_);
  request.set_operation(IterateRequest::OP_SET);
  request.set_value(std::string(value));
  if (!stream_->Write(request)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Write failed: ", message));
  }
  IterateResponse response;
  if (!stream_->Read(&response)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Read failed: ", message));
  }
  return MakeStatusFromProto(response.status());
}

Status RemoteDBMIteratorImpl::Remove() {
  std::shared_lock<SpinSharedMutex> lock(dbm_->mutex_);
  if (dbm_->stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  if (!healthy_.load()) {
    return Status(Status::PRECONDITION_ERROR, "unhealthy stream");
  }
  context_.set_deadline(std::chrono::system_clock::now() + std::chrono::microseconds(
      static_cast<int64_t>(dbm_->timeout_ * 1000000)));
  IterateRequest request;
  request.set_dbm_index(dbm_->dbm_index_);
  request.set_operation(IterateRequest::OP_REMOVE);
  if (!stream_->Write(request)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Write failed: ", message));
  }
  IterateResponse response;
  if (!stream_->Read(&response)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Read failed: ", message));
  }
  return MakeStatusFromProto(response.status());
}

RemoteDBMReplicatorImpl::RemoteDBMReplicatorImpl(RemoteDBMImpl* dbm)
    : dbm_(dbm), context_(), stream_(nullptr), healthy_(true), server_id_(-1) {
  if (healthy_.load()) {
    std::lock_guard<SpinSharedMutex> lock(dbm_->mutex_);
    dbm_->replicators_.emplace_back(this);
  }
  std::shared_lock<SpinSharedMutex> lock(dbm_->mutex_);
  context_.set_deadline(std::chrono::system_clock::now() + std::chrono::microseconds(
      static_cast<int64_t>(dbm_->timeout_ * 1000000)));
}

RemoteDBMReplicatorImpl::~RemoteDBMReplicatorImpl() {
  if (dbm_ != nullptr) {
    std::lock_guard<SpinSharedMutex> lock(dbm_->mutex_);
    dbm_->replicators_.remove(this);
  }
}

void RemoteDBMReplicatorImpl::Cancel() {
  healthy_.store(false);
  context_.TryCancel();
}

int32_t RemoteDBMReplicatorImpl::GetMasterServerID() {
  return server_id_;
}

Status RemoteDBMReplicatorImpl::Start(
    int64_t min_timestamp, int32_t server_id, double wait_time) {
  std::shared_lock<SpinSharedMutex> lock(dbm_->mutex_);
  if (dbm_->stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  if (stream_ != nullptr) {
    return Status(Status::PRECONDITION_ERROR, "started replicator");
  }
  if (!healthy_.load()) {
    return Status(Status::PRECONDITION_ERROR, "unhealthy stream");
  }
  context_.set_deadline(std::chrono::system_clock::now() + std::chrono::microseconds(
      static_cast<int64_t>(dbm_->timeout_ * 1000000)));
  ReplicateRequest request;
  request.set_min_timestamp(min_timestamp);
  request.set_server_id(server_id);
  request.set_wait_time(wait_time);
  stream_ = dbm_->stub_->Replicate(&context_, request);
  ReplicateResponse response;
  if (!stream_->Read(&response)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Read failed: ", message));
  }
  if (response.op_type() != ReplicateResponse::OP_NOOP) {
    return Status(Status::BROKEN_DATA_ERROR, "invalid operation type");
  }
  server_id_ = response.server_id();
  return Status(Status::SUCCESS);
}

Status RemoteDBMReplicatorImpl::Read(int64_t* timestamp, RemoteDBM::ReplicateLog* op) {
  std::shared_lock<SpinSharedMutex> lock(dbm_->mutex_);
  if (dbm_->stub_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not connected database");
  }
  if (stream_ == nullptr) {
    return Status(Status::PRECONDITION_ERROR, "not started replicator");
  }
  if (!healthy_.load()) {
    return Status(Status::PRECONDITION_ERROR, "unhealthy stream");
  }
  ReplicateResponse response;
  if (!stream_->Read(&response)) {
    healthy_.store(false);
    const std::string message = GRPCStatusString(stream_->Finish());
    return Status(Status::NETWORK_ERROR, StrCat("Read failed: ", message));
  }
  *timestamp = response.timestamp();
  delete[] op->buffer_;
  switch (response.op_type()) {
    case ReplicateResponse::OP_SET:
      op->op_type = DBMUpdateLoggerMQ::OP_SET;
      break;
    case ReplicateResponse::OP_REMOVE:
      op->op_type = DBMUpdateLoggerMQ::OP_REMOVE;
      break;
    case ReplicateResponse::OP_CLEAR:
      op->op_type = DBMUpdateLoggerMQ::OP_CLEAR;
      break;
    default:
      op->op_type = DBMUpdateLoggerMQ::OP_VOID;
      break;
  }
  op->server_id = response.server_id();
  op->dbm_index = response.dbm_index();
  op->buffer_ = new char[response.key().size() + response.value().size() + 1];
  char* wp = op->buffer_;
  std::memcpy(wp, response.key().data(), response.key().size());
  op->key = std::string_view(wp, response.key().size());
  wp += response.key().size();
  std::memcpy(wp, response.value().data(), response.value().size());
  op->value = std::string_view(wp, response.value().size());
  return MakeStatusFromProto(response.status());
}

RemoteDBM::RemoteDBM() : impl_(nullptr) {
  impl_ = new RemoteDBMImpl();
}

RemoteDBM::~RemoteDBM() {
  delete impl_;
}

void RemoteDBM::InjectStub(void* stub) {
  impl_->InjectStub(stub);
}

Status RemoteDBM::Connect(const std::string& address, double timeout) {
  return impl_->Connect(address, timeout);
}

Status RemoteDBM::Disconnect() {
  return impl_->Disconnect();
}

Status RemoteDBM::SetDBMIndex(int32_t dbm_index) {
  return impl_->SetDBMIndex(dbm_index);
}

Status RemoteDBM::Echo(std::string_view message, std::string* echo) {
  return impl_->Echo(message, echo);
}

Status RemoteDBM::Inspect(std::vector<std::pair<std::string, std::string>>* records) {
  return impl_->Inspect(records);
}

Status RemoteDBM::Get(std::string_view key, std::string* value) {
  return impl_->Get(key, value);
}

Status RemoteDBM::GetMulti(
    const std::vector<std::string_view>& keys, std::map<std::string, std::string>* records) {
  return impl_->GetMulti(keys, records);
}

Status RemoteDBM::Set(std::string_view key, std::string_view value, bool overwrite) {
  return impl_->Set(key, value, overwrite);
}

Status RemoteDBM::SetMulti(
    const std::map<std::string_view, std::string_view>& records, bool overwrite) {
  return impl_->SetMulti(records, overwrite);
}

Status RemoteDBM::Remove(std::string_view key) {
  return impl_->Remove(key);
}

Status RemoteDBM::RemoveMulti(const std::vector<std::string_view>& keys) {
  return impl_->RemoveMulti(keys);
}

Status RemoteDBM::Append(std::string_view key, std::string_view value, std::string_view delim) {
  return impl_->Append(key, value, delim);
}

Status RemoteDBM::AppendMulti(
    const std::map<std::string_view, std::string_view>& records, std::string_view delim) {
  return impl_->AppendMulti(records, delim);
}

Status RemoteDBM::CompareExchange(std::string_view key, std::string_view expected,
                                  std::string_view desired) {
  return impl_->CompareExchange(key, expected, desired);
}

Status RemoteDBM::Increment(
    std::string_view key, int64_t increment, int64_t* current, int64_t initial) {
  return impl_->Increment(key, increment, current, initial);
}

Status RemoteDBM::CompareExchangeMulti(
    const std::vector<std::pair<std::string_view, std::string_view>>& expected,
    const std::vector<std::pair<std::string_view, std::string_view>>& desired) {
  return impl_->CompareExchangeMulti(expected, desired);
}

Status RemoteDBM::Count(int64_t* count) {
  return impl_->Count(count);
}

Status RemoteDBM::GetFileSize(int64_t* file_size) {
  return impl_->GetFileSize(file_size);
}

Status RemoteDBM::Clear() {
  return impl_->Clear();
}

Status RemoteDBM::Rebuild(const std::map<std::string, std::string>& params) {
  return impl_->Rebuild(params);
}

Status RemoteDBM::ShouldBeRebuilt(bool* tobe) {
  return impl_->ShouldBeRebuilt(tobe);
}

Status RemoteDBM::Synchronize(bool hard, const std::map<std::string, std::string>& params) {
  return impl_->Synchronize(hard, params);
}

Status RemoteDBM::SearchModal(
    std::string_view mode, std::string_view pattern,
    std::vector<std::string>* matched, size_t capacity) {
  return impl_->SearchModal(mode, pattern, matched, capacity);
}

Status RemoteDBM::ChangeMaster(std::string_view master, double timestamp_skew) {
  return impl_->ChangeMaster(master, timestamp_skew);
}

std::unique_ptr<RemoteDBM::Stream> RemoteDBM::MakeStream() {
  std::unique_ptr<RemoteDBM::Stream> iter(new RemoteDBM::Stream(impl_));
  return iter;
}

std::unique_ptr<RemoteDBM::Iterator> RemoteDBM::MakeIterator() {
  std::unique_ptr<RemoteDBM::Iterator> iter(new RemoteDBM::Iterator(impl_));
  return iter;
}

std::unique_ptr<RemoteDBM::Replicator> RemoteDBM::MakeReplicator() {
  std::unique_ptr<RemoteDBM::Replicator> iter(new RemoteDBM::Replicator(impl_));
  return iter;
}

RemoteDBM::Stream::Stream(RemoteDBMImpl* dbm_impl) {
  impl_ = new RemoteDBMStreamImpl(dbm_impl);
}

RemoteDBM::Stream::~Stream() {
  delete impl_;
}

void RemoteDBM::Stream::Cancel() {
  impl_->Cancel();
}

Status RemoteDBM::Stream::Echo(std::string_view message, std::string* echo) {
  return impl_->Echo(message, echo);
}

Status RemoteDBM::Stream::Get(std::string_view key, std::string* value) {
  return impl_->Get(key, value);
}

Status RemoteDBM::Stream::Set(std::string_view key, std::string_view value,
                              bool overwrite, bool ignore_result) {
  return impl_->Set(key, value, overwrite, ignore_result);
}

Status RemoteDBM::Stream::Remove(std::string_view key, bool ignore_result) {
  return impl_->Remove(key, ignore_result);
}

Status RemoteDBM::Stream::Append(
    std::string_view key, std::string_view value, std::string_view delim,
    bool ignore_result) {
  return impl_->Append(key, value, delim, ignore_result);
}

Status RemoteDBM::Stream::CompareExchange(
    std::string_view key, std::string_view expected, std::string_view desired) {
  return impl_->CompareExchange(key, expected, desired);
}

Status RemoteDBM::Stream::Increment(
    std::string_view key, int64_t increment,
    int64_t* current, int64_t initial, bool ignore_result) {
  return impl_->Increment(key, increment, current, initial, ignore_result);
}

RemoteDBM::Iterator::Iterator(RemoteDBMImpl* dbm_impl) {
  impl_ = new RemoteDBMIteratorImpl(dbm_impl);
}

RemoteDBM::Iterator::~Iterator() {
  delete impl_;
}

void RemoteDBM::Iterator::Cancel() {
  impl_->Cancel();
}

Status RemoteDBM::Iterator::First() {
  return impl_->First();
}

Status RemoteDBM::Iterator::Last() {
  return impl_->Last();
}

Status RemoteDBM::Iterator::Jump(std::string_view key) {
  return impl_->Jump(key);
}

Status RemoteDBM::Iterator::JumpLower(std::string_view key, bool inclusive) {
  return impl_->JumpLower(key, inclusive);
}

Status RemoteDBM::Iterator::JumpUpper(std::string_view key, bool inclusive) {
  return impl_->JumpUpper(key, inclusive);
}

Status RemoteDBM::Iterator::Next() {
  return impl_->Next();
}

Status RemoteDBM::Iterator::Previous() {
  return impl_->Previous();
}

Status RemoteDBM::Iterator::Get(std::string* key, std::string* value) {
  return impl_->Get(key, value);
}

Status RemoteDBM::Iterator::Set(std::string_view value) {
  return impl_->Set(value);
}

Status RemoteDBM::Iterator::Remove() {
  return impl_->Remove();
}

RemoteDBM::ReplicateLog::ReplicateLog() : buffer_(nullptr) {}

RemoteDBM::ReplicateLog::~ReplicateLog() {
  delete[] buffer_;
}

RemoteDBM::Replicator::Replicator(RemoteDBMImpl* dbm_impl) {
  impl_ = new RemoteDBMReplicatorImpl(dbm_impl);
}

RemoteDBM::Replicator::~Replicator() {
  delete impl_;
}

void RemoteDBM::Replicator::Cancel() {
  impl_->Cancel();
}

int32_t RemoteDBM::Replicator::GetMasterServerID() {
  return impl_->GetMasterServerID();
}

Status RemoteDBM::Replicator::Start(int64_t min_timestamp, int32_t server_id, double timeout) {
  return impl_->Start(min_timestamp, server_id, timeout);
}

Status RemoteDBM::Replicator::Read(int64_t* timestamp, ReplicateLog* op) {
  assert(timestamp != nulptr && op != nullptr);
  return impl_->Read(timestamp, op);
}

}  // namespace tkrzw

// END OF FILE
