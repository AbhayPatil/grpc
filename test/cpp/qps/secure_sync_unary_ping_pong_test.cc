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

#include <set>

#include <grpc/support/log.h>

#include "test/core/util/test_config.h"
#include "test/cpp/qps/benchmark_config.h"
#include "test/cpp/qps/driver.h"
#include "test/cpp/qps/report.h"
#include "test/cpp/qps/server.h"
#include "test/cpp/util/test_config.h"
#include "test/cpp/util/test_credentials_provider.h"

namespace grpc {
namespace testing {

static const int WARMUP = 2;
static const int BENCHMARK = 4;

static void RunAsynchronousUnaryPingPong() {
  gpr_log(GPR_INFO, "Running Synchronous Unary Ping Pong");

  ClientConfig client_config;
  client_config.set_client_type(ASYNC_CLIENT);
  client_config.set_outstanding_rpcs_per_channel(1);
  client_config.set_client_channels(1);
  client_config.set_rpc_type(UNARY);
  client_config.mutable_load_params()->mutable_closed_loop();
  
  // client_config.set_async_client_threads(1);
  // client_config.set_client_processes(0);
  // client_config.set_threads_per_cq(0);

  // auto* h = client_config.mutable_histogram_params();
  // h->set_resolution(0.01);
  // h->set_max_possible(60000000000.0);
  

  ServerConfig server_config;
  server_config.set_server_type(ASYNC_SERVER);

  // // Set up security params
  // SecurityParams security;
  // security.set_use_test_ca(true);
  // security.set_server_host_override("foo.test.google.fr");
  // client_config.mutable_security_params()->CopyFrom(security);
  // server_config.mutable_security_params()->CopyFrom(security);

  const auto result =
      RunScenario(client_config, 1, server_config, 1, WARMUP, BENCHMARK, -2, "",
                  kInsecureCredentialsType, {}, false, 0);

  GetReporter()->ReportQPS(*result);
  GetReporter()->ReportLatency(*result);

  gpr_log(GPR_INFO, "Result Summary:\n");
  gpr_log(GPR_INFO, result->summary().DebugString().c_str());

  gpr_log(GPR_INFO, "Server Config:");
  gpr_log(GPR_INFO, server_config.DebugString().c_str());

  gpr_log(GPR_INFO, "Client Config:");
  gpr_log(GPR_INFO, client_config.DebugString().c_str());

}

}  // namespace testing
}  // namespace grpc

int main(int argc, char** argv) {
  grpc::testing::TestEnvironment env(&argc, argv);
  grpc::testing::InitTest(&argc, &argv, true);

  grpc::testing::RunAsynchronousUnaryPingPong();
  return 0;
}
