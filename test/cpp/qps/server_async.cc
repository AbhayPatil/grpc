/*
 *
 * Copyright 2015 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <algorithm>
#include <forward_list>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include <grpc/grpc.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpcpp/generic/async_generic_service.h>
#include <grpcpp/resource_quota.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>
#include <grpcpp/support/config.h>

#include "src/core/lib/gprpp/host_port.h"
#include "src/core/lib/surface/completion_queue.h"
#include "src/proto/grpc/testing/benchmark_service.grpc.pb.h"
#include "test/core/util/test_config.h"
#include "test/cpp/qps/qps_server_builder.h"
#include "test/cpp/qps/server.h"

namespace grpc {
namespace testing {

template <class RequestType, class ResponseType, class ServiceType,
          class ServerContextType>
class AsyncQpsServerTest final : public grpc::testing::Server {
 public:
  AsyncQpsServerTest(
      const ServerConfig& config,
      std::function<void(ServerBuilder*, ServiceType*)> register_service,
      std::function<void(ServiceType*, ServerContextType*, RequestType*,
                         ServerAsyncResponseWriter<ResponseType>*,
                         CompletionQueue*, ServerCompletionQueue*, void*)>
          request_unary_function,
      std::function<grpc::Status(const PayloadConfig&, RequestType*,
                                 ResponseType*)>
          process_rpc)
      : Server(config) {
    std::unique_ptr<ServerBuilder> builder = CreateQpsServerBuilder();

    auto port_num = port();
    // Negative port number means inproc server, so no listen port needed
    if (port_num >= 0) {
      std::string server_address = grpc_core::JoinHostPort("::", port_num);
      builder->AddListeningPort(server_address.c_str(),
                                Server::CreateServerCredentials(config),
                                &port_num);
    }

    register_service(builder.get(), &async_service_);

    int num_threads = config.async_server_threads();
    if (num_threads <= 0) {  // dynamic sizing
      num_threads = std::min(64, cores());
      gpr_log(GPR_INFO,
              "Sizing async server to %d threads. Defaults to number of cores "
              "in machine or 64 threads if machine has more than 64 cores to "
              "avoid OOMs.",
              num_threads);
    }

    int tpc = std::max(1, config.threads_per_cq());  // 1 if unspecified
    int num_cqs = (num_threads + tpc - 1) / tpc;     // ceiling operator
    for (int i = 0; i < num_cqs; i++) {
      srv_cqs_.emplace_back(builder->AddCompletionQueue());
    }
    for (int i = 0; i < num_threads; i++) {
      cq_.emplace_back(i % srv_cqs_.size());
    }

    ApplyConfigToBuilder(config, builder.get());

    server_ = builder->BuildAndStart();
    if (server_ == nullptr) {
      gpr_log(GPR_ERROR, "Server: Fail to BuildAndStart(port=%d)", port_num);
    } else {
      gpr_log(GPR_INFO, "Server: BuildAndStart(port=%d)", port_num);
    }

    auto process_rpc_bound =
        std::bind(process_rpc, config.payload_config(), std::placeholders::_1,
                  std::placeholders::_2);

    for (int i = 0; i < 5000; i++) {
      for (int j = 0; j < num_cqs; j++) {
        if (request_unary_function) {
          auto request_unary = std::bind(
              request_unary_function, &async_service_, std::placeholders::_1,
              std::placeholders::_2, std::placeholders::_3, srv_cqs_[j].get(),
              srv_cqs_[j].get(), std::placeholders::_4);
          contexts_.emplace_back(
              new ServerRpcContextUnaryImpl(request_unary, process_rpc_bound));
        }

      }
    }

    for (int i = 0; i < num_threads; i++) {
      shutdown_state_.emplace_back(new PerThreadShutdownState());
      threads_.emplace_back(&AsyncQpsServerTest::ThreadFunc, this, i);
    }
  }
  ~AsyncQpsServerTest() override {
    for (auto ss = shutdown_state_.begin(); ss != shutdown_state_.end(); ++ss) {
      std::lock_guard<std::mutex> lock((*ss)->mutex);
      (*ss)->shutdown = true;
    }
    // TODO(vjpai): Remove the following deadline and allow full proper
    // shutdown.
    server_->Shutdown(std::chrono::system_clock::now() +
                      std::chrono::seconds(3));
    for (auto cq = srv_cqs_.begin(); cq != srv_cqs_.end(); ++cq) {
      (*cq)->Shutdown();
    }
    for (auto thr = threads_.begin(); thr != threads_.end(); thr++) {
      thr->join();
    }
    for (auto cq = srv_cqs_.begin(); cq != srv_cqs_.end(); ++cq) {
      bool ok;
      void* got_tag;
      while ((*cq)->Next(&got_tag, &ok)) {
      }
    }
  }

  int GetPollCount() override {
    int count = 0;
    for (auto cq = srv_cqs_.begin(); cq != srv_cqs_.end(); cq++) {
      count += grpc_get_cq_poll_num((*cq)->cq());
    }
    return count;
  }

  std::shared_ptr<Channel> InProcessChannel(
      const ChannelArguments& args) override {
    return server_->InProcessChannel(args);
  }

 private:
  void ThreadFunc(int thread_idx) {
    // Wait until work is available or we are shutting down
    bool ok;
    void* got_tag;
    auto cli_cqs_index = cq_[thread_idx];
    ServerRpcContext* ctx;

    while (srv_cqs_[cli_cqs_index]->Next(&got_tag, &ok)) {
      if (shutdown_state_[thread_idx]->shutdown) {
        return;
      }

      if (!ok) continue;
      ctx = detag(got_tag);
      if (!ctx->RunNextState(ok)) {
        ctx->Reset();
      }
    }

  }

  class ServerRpcContext {
   public:
    ServerRpcContext() {}
    void lock() { mu_.lock(); }
    void unlock() { mu_.unlock(); }
    virtual ~ServerRpcContext(){};
    virtual bool RunNextState(bool) = 0;  // next state, return false if done
    virtual void Reset() = 0;             // start this back at a clean state
   private:
    std::mutex mu_;
  };
  static void* tag(ServerRpcContext* func) { return static_cast<void*>(func); }
  static ServerRpcContext* detag(void* tag) {
    return static_cast<ServerRpcContext*>(tag);
  }

  class ServerRpcContextUnaryImpl final : public ServerRpcContext {
   public:
    ServerRpcContextUnaryImpl(
        std::function<void(ServerContextType*, RequestType*,
                           grpc::ServerAsyncResponseWriter<ResponseType>*,
                           void*)>
            request_method,
        std::function<grpc::Status(RequestType*, ResponseType*)> invoke_method)
        : srv_ctx_(new ServerContextType),
          next_state_(&ServerRpcContextUnaryImpl::invoker),
          request_method_(request_method),
          invoke_method_(invoke_method),
          response_writer_(srv_ctx_.get()) {
      request_method_(srv_ctx_.get(), &req_, &response_writer_,
                      AsyncQpsServerTest::tag(this));
    }
    ~ServerRpcContextUnaryImpl() override {}
    bool RunNextState(bool ok) override { return (this->*next_state_)(ok); }
    void Reset() override {
      srv_ctx_.reset(new ServerContextType);
      req_ = RequestType();
      response_writer_ =
          grpc::ServerAsyncResponseWriter<ResponseType>(srv_ctx_.get());

      // Then request the method
      next_state_ = &ServerRpcContextUnaryImpl::invoker;
      request_method_(srv_ctx_.get(), &req_, &response_writer_,
                      AsyncQpsServerTest::tag(this));
    }

   private:
    bool finisher(bool) { return false; }
    bool invoker(bool ok) {
      if (!ok) {
        return false;
      }

      // Call the RPC processing function
      grpc::Status status = invoke_method_(&req_, &response_);

      // Have the response writer work and invoke on_finish when done
      next_state_ = &ServerRpcContextUnaryImpl::finisher;
      response_writer_.Finish(response_, status, AsyncQpsServerTest::tag(this));
      return true;
    }
    std::unique_ptr<ServerContextType> srv_ctx_;
    RequestType req_;
    ResponseType response_;
    bool (ServerRpcContextUnaryImpl::*next_state_)(bool);
    std::function<void(ServerContextType*, RequestType*,
                       grpc::ServerAsyncResponseWriter<ResponseType>*, void*)>
        request_method_;
    std::function<grpc::Status(RequestType*, ResponseType*)> invoke_method_;
    grpc::ServerAsyncResponseWriter<ResponseType> response_writer_;
  };

  std::vector<std::thread> threads_;
  std::unique_ptr<grpc::Server> server_;
  std::vector<std::unique_ptr<grpc::ServerCompletionQueue>> srv_cqs_;
  std::vector<int> cq_;
  ServiceType async_service_;
  std::vector<std::unique_ptr<ServerRpcContext>> contexts_;

  struct PerThreadShutdownState {
    mutable std::mutex mutex;
    bool shutdown;
    PerThreadShutdownState() : shutdown(false) {}
  };

  std::vector<std::unique_ptr<PerThreadShutdownState>> shutdown_state_;
};

static void RegisterBenchmarkService(ServerBuilder* builder,
                                     BenchmarkService::AsyncService* service) {
  builder->RegisterService(service);
}
static void RegisterGenericService(ServerBuilder* builder,
                                   grpc::AsyncGenericService* service) {
  builder->RegisterAsyncGenericService(service);
}

static Status ProcessSimpleRPC(const PayloadConfig&, SimpleRequest* request,
                               SimpleResponse* response) {
  if (request->response_size() > 0) {
    // if (!Server::SetPayload(request->response_type(), request->response_size(),
    //                         response->mutable_payload())) {
    //   return Status(grpc::StatusCode::INTERNAL, "Error creating payload.");
    // }
  }
  // We are done using the request. Clear it to reduce working memory.
  // This proves to reduce cache misses in large message size cases.
  request->Clear();
  return Status::OK;
}

static Status ProcessGenericRPC(const PayloadConfig& payload_config,
                                ByteBuffer* request, ByteBuffer* response) {
  return Status::OK;
}

std::unique_ptr<Server> CreateAsyncServer(const ServerConfig& config) {
  return std::unique_ptr<Server>(
      new AsyncQpsServerTest<SimpleRequest, SimpleResponse,
                             BenchmarkService::AsyncService,
                             grpc::ServerContext>(
          config, RegisterBenchmarkService,
          &BenchmarkService::AsyncService::RequestUnaryCall,
          ProcessSimpleRPC));
}
std::unique_ptr<Server> CreateAsyncGenericServer(const ServerConfig& config) {
  return nullptr;
}

}  // namespace testing
}  // namespace grpc
