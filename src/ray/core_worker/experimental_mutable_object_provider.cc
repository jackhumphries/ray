// Copyright 2024 The Ray Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ray/core_worker/experimental_mutable_object_provider.h"

namespace ray {
namespace core {
namespace experimental {

MutableObjectProvider::MutableObjectProvider(
    std::shared_ptr<plasma::PlasmaClientInterface> plasma, RayletFactory factory)
    : plasma_(plasma),
      raylet_client_factory_(factory),
      io_work_(io_service_),
      client_call_manager_(std::make_unique<rpc::ClientCallManager>(io_service_)),
      io_thread_([this]() { RunIOService(); }) {}

MutableObjectProvider::~MutableObjectProvider() {
  io_service_.stop();
  object_manager_.SetError();

  RAY_CHECK(io_thread_.joinable());
  io_thread_.join();
}

void MutableObjectProvider::RegisterWriterChannel(const ObjectID &object_id,
                                                  const NodeID &node_id) {
  {
    std::unique_ptr<plasma::MutableObject> object;
    RAY_CHECK_OK(plasma_->GetExperimentalMutableObject(object_id, &object));
    RAY_CHECK_OK(
        object_manager_.RegisterChannel(object_id, std::move(object), /*reader=*/false));
    // `object` is now a nullptr.
  }

  // Start a thread that repeatedly listens for values on this object and then sends them
  // via RPC to the remote reader.
  std::shared_ptr<MutableObjectReaderInterface> reader = raylet_client_factory_(node_id);
  RAY_CHECK(reader);
  // TODO(jhumphri): Extend this to support multiple channels. Currently, we must have
  // one thread per channel because the thread blocks on the channel semaphore.
  io_service_.post([this, object_id, reader]() { PollWriterClosure(object_id, reader); },
                   "experimental::MutableObjectProvider.PollWriter");
}

void MutableObjectProvider::RegisterReaderChannel(const ObjectID &object_id) {
  std::unique_ptr<plasma::MutableObject> object;
  RAY_CHECK_OK(plasma_->GetExperimentalMutableObject(object_id, &object));
  RAY_CHECK_OK(
      object_manager_.RegisterChannel(object_id, std::move(object), /*reader=*/true));
  // `object` is now a nullptr.
}

void MutableObjectProvider::HandleRegisterMutableObject(const ObjectID &object_id,
                                                        int64_t num_readers,
                                                        const ObjectID &local_object_id) {
  absl::MutexLock guard(&cross_node_map_lock_);
  LocalInfo info = {.num_readers = num_readers, .local_object_id = local_object_id};
  bool success = cross_node_map_.insert({object_id, info}).second;
  RAY_CHECK(success);
  RegisterReaderChannel(local_object_id);
}

void MutableObjectProvider::HandlePushMutableObject(
    const rpc::PushMutableObjectRequest &request, rpc::PushMutableObjectReply *reply) {
  LocalInfo info;
  {
    const ObjectID object_id = ObjectID::FromBinary(request.object_id());
    absl::MutexLock guard(&cross_node_map_lock_);
    auto it = cross_node_map_.find(object_id);
    RAY_CHECK(it != cross_node_map_.end());
    info = it->second;
  }
  size_t data_size = request.data_size();
  size_t metadata_size = request.metadata_size();

  // Copy both the data and metadata to a local channel.
  std::shared_ptr<Buffer> data;
  const uint8_t *metadata_ptr =
      reinterpret_cast<const uint8_t *>(request.data().data()) + request.data_size();
  RAY_CHECK_OK(object_manager_.WriteAcquire(info.local_object_id,
                                            data_size,
                                            metadata_ptr,
                                            metadata_size,
                                            info.num_readers,
                                            data));
  RAY_CHECK(data);

  size_t total_size = data_size + metadata_size;
  // The buffer has the data immediately followed by the metadata. `WriteAcquire()`
  // above checks that the buffer size is at least `total_size`.
  memcpy(data->Data(), request.data().data(), total_size);
  RAY_CHECK_OK(object_manager_.WriteRelease(info.local_object_id));
}

void MutableObjectProvider::PollWriterClosure(
    const ObjectID &object_id, std::shared_ptr<MutableObjectReaderInterface> reader) {
  std::shared_ptr<RayObject> object;
  Status status = object_manager_.ReadAcquire(object_id, object);
  // Check if the thread returned from ReadAcquire() because the process is exiting, not
  // because there is something to read.
  if (status.code() == StatusCode::IOError) {
    // The process is exiting.
    return;
  }
  RAY_CHECK_EQ(static_cast<int>(status.code()), static_cast<int>(StatusCode::OK));

  RAY_CHECK(object->GetData());
  RAY_CHECK(object->GetMetadata());
  reader->PushMutableObject(
      object_id,
      object->GetData()->Size(),
      object->GetMetadata()->Size(),
      object->GetData()->Data(),
      [this, object_id, reader](const Status &status,
                                const rpc::PushMutableObjectReply &reply) {
        RAY_CHECK_OK(object_manager_.ReadRelease(object_id));
        PollWriterClosure(object_id, reader);
      });
}

void MutableObjectProvider::RunIOService() {
  // TODO(jhumphri): Decompose this.
#ifndef _WIN32
  // Block SIGINT and SIGTERM so they will be handled by the main thread.
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &mask, NULL);
#endif

  SetThreadName("worker.channel_io");
  io_service_.run();
  RAY_LOG(INFO) << "Core worker channel io service stopped.";
}

}  // namespace experimental
}  // namespace core
}  // namespace ray
