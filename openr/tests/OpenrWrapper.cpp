/**
 * Copyright (c) 2014-present, Facebook, Inc.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "OpenrWrapper.h"

#include <folly/MapUtil.h>
#include <re2/re2.h>
#include <re2/set.h>

#include <openr/config/tests/Utils.h>

namespace openr {

template <class Serializer>
OpenrWrapper<Serializer>::OpenrWrapper(
    fbzmq::Context& context,
    std::string nodeId,
    bool v4Enabled,
    std::chrono::seconds kvStoreDbSyncInterval,
    std::chrono::milliseconds sparkHoldTime,
    std::chrono::milliseconds sparkKeepAliveTime,
    std::chrono::milliseconds sparkFastInitKeepAliveTime,
    std::chrono::seconds linkMonitorAdjHoldTime,
    std::chrono::milliseconds linkFlapInitialBackoff,
    std::chrono::milliseconds linkFlapMaxBackoff,
    std::chrono::seconds fibColdStartDuration,
    std::shared_ptr<IoProvider> ioProvider,
    int32_t systemPort,
    uint32_t memLimit,
    bool per_prefix_keys)
    : context_(context),
      nodeId_(nodeId),
      ioProvider_(std::move(ioProvider)),
      monitorSubmitUrl_(folly::sformat("inproc://{}-monitor-submit", nodeId_)),
      monitorPubUrl_(folly::sformat("inproc://{}-monitor-pub", nodeId_)),
      kvStoreGlobalCmdUrl_(
          folly::sformat("inproc://{}-kvstore-cmd-global", nodeId_)),
      platformPubUrl_(folly::sformat("inproc://{}-platform-pub", nodeId_)),
      platformPubSock_(context),
      systemPort_(systemPort),
      per_prefix_keys_(per_prefix_keys) {
  // create config
  auto tConfig = getBasicOpenrConfig(
      nodeId_,
      v4Enabled,
      false /*enableSegmentRouting*/,
      false /*orderedFibProgramming*/,
      true /*dryrun*/);
  tConfig.kvstore_config.sync_interval_s = kvStoreDbSyncInterval.count();
  config_ = std::make_shared<Config>(tConfig);

  // create zmq monitor
  monitor_ = std::make_unique<fbzmq::ZmqMonitor>(
      MonitorSubmitUrl{monitorSubmitUrl_},
      MonitorPubUrl{monitorPubUrl_},
      context_);

  // create and start config-store thread
  configStore_ = std::make_unique<PersistentStore>(
      nodeId_,
      folly::sformat("/tmp/{}_aq_config_store.bin", nodeId_),
      context_);
  std::thread configStoreThread([this]() noexcept {
    VLOG(1) << nodeId_ << " ConfigStore running.";
    configStore_->run();
    VLOG(1) << nodeId_ << " ConfigStore stopped.";
  });
  configStore_->waitUntilRunning();
  allThreads_.emplace_back(std::move(configStoreThread));

  // create and start kvstore thread
  kvStore_ = std::make_unique<KvStore>(
      context_,
      kvStoreUpdatesQueue_,
      peerUpdatesQueue_.getReader(),
      KvStoreGlobalCmdUrl{kvStoreGlobalCmdUrl_},
      MonitorSubmitUrl{monitorSubmitUrl_},
      config_,
      std::nullopt /* ip-tos */,
      std::unordered_map<std::string, thrift::PeerSpec>{});
  std::thread kvStoreThread([this]() noexcept {
    VLOG(1) << nodeId_ << " KvStore running.";
    kvStore_->run();
    VLOG(1) << nodeId_ << " KvStore stopped.";
  });
  kvStore_->waitUntilRunning();
  allThreads_.emplace_back(std::move(kvStoreThread));

  // kvstore client
  kvStoreClient_ = std::make_unique<KvStoreClientInternal>(
      &eventBase_, nodeId_, kvStore_.get());

  // Subscribe our own prefixDb
  kvStoreClient_->subscribeKey(
      folly::sformat("prefix:{}", nodeId_),
      [&](const std::string& /* key */,
          std::optional<thrift::Value> value) noexcept {
        if (!value.has_value()) {
          return;
        }
        // Parse PrefixDb
        auto prefixDb = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
            value.value().value_ref().value(), serializer_);

        SYNCHRONIZED(ipPrefix_) {
          bool received = false;

          for (auto& prefix : prefixDb.prefixEntries) {
            if (prefix.type == thrift::PrefixType::PREFIX_ALLOCATOR) {
              received = true;
              ipPrefix_ = prefix.prefix;
              break;
            }
          }
          if (!received) {
            ipPrefix_ = std::nullopt;
          }
        }
      },
      false);

  //
  // create spark
  //
  spark_ = std::make_unique<Spark>(
      "terragraph", // domain name
      nodeId_, // node name
      static_cast<uint16_t>(6666), // multicast port
      sparkHoldTime, // hold time ms
      sparkKeepAliveTime, // keep alive ms
      sparkFastInitKeepAliveTime, // fastInitKeepAliveTime ms
      std::chrono::milliseconds{0}, /* spark2_hello_time */
      std::chrono::milliseconds{0}, /* spark2_hello_fast_init_time */
      std::chrono::milliseconds{0}, /* spark2_handshake_time */
      std::chrono::milliseconds{0}, /* spark2_heartbeat_time */
      std::chrono::milliseconds{0}, /* spark2_negotiate_hold_time */
      std::chrono::milliseconds{0}, /* spark2_heartbeat_hold_time */
      std::nullopt, // ip-tos
      v4Enabled, // enable v4
      interfaceUpdatesQueue_.getReader(),
      neighborUpdatesQueue_,
      KvStoreCmdPort{0},
      OpenrCtrlThriftPort{0},
      std::make_pair(
          Constants::kOpenrVersion, Constants::kOpenrSupportedVersion),
      context_,
      ioProvider_);

  //
  // create link monitor
  //
  std::string ifName = "vethLMTest_" + nodeId_;
  std::vector<thrift::IpPrefix> networks;
  networks.emplace_back(toIpPrefix(folly::IPAddress::createNetwork("::/0")));
  re2::RE2::Options options;
  options.set_case_sensitive(false);
  std::string regexErr;
  auto includeRegexList =
      std::make_unique<re2::RE2::Set>(options, re2::RE2::ANCHOR_BOTH);
  includeRegexList->Add(ifName + ".*", &regexErr);
  includeRegexList->Compile();
  std::unique_ptr<re2::RE2::Set> excludeRegexList;
  std::unique_ptr<re2::RE2::Set> redistRegexList;

  linkMonitor_ = std::make_unique<LinkMonitor>(
      context_,
      nodeId_,
      static_cast<int32_t>(60099), // platfrom pub port
      kvStore_.get(),
      std::move(includeRegexList),
      std::move(excludeRegexList),
      // redistribute interface names
      std::move(redistRegexList),
      networks,
      false, /* use rtt metric */
      false /* enable perf measurement */,
      false /* enable v4 */,
      true /* enable segment routing */,
      false /* prefix type mpls */,
      false /* prefix fwd algo KSP2_ED_ECMP */,
      AdjacencyDbMarker{"adj:"},
      interfaceUpdatesQueue_,
      peerUpdatesQueue_,
      neighborUpdatesQueue_.getReader(),
      MonitorSubmitUrl{monitorSubmitUrl_},
      configStore_.get(),
      false,
      prefixUpdatesQueue_,
      PlatformPublisherUrl{platformPubUrl_},
      linkMonitorAdjHoldTime,
      linkFlapInitialBackoff,
      linkFlapMaxBackoff,
      Constants::kKvStoreDbTtl);

  //
  // Create prefix manager
  //
  prefixManager_ = std::make_unique<PrefixManager>(
      nodeId_,
      prefixUpdatesQueue_.getReader(),
      configStore_.get(),
      kvStore_.get(),
      PrefixDbMarker{"prefix:"},
      per_prefix_keys_ /* create IP prefix keys */,
      false /* prefix-mananger perf measurement */,
      std::chrono::seconds(0),
      Constants::kKvStoreDbTtl);

  //
  // create decision
  //
  decision_ = std::make_unique<Decision>(
      config_,
      true, // computeLfaPaths
      false, // bgpDryRun
      std::chrono::milliseconds(10),
      std::chrono::milliseconds(250),
      kvStoreUpdatesQueue_.getReader(),
      staticRoutesQueue_.getReader(),
      routeUpdatesQueue_,
      context_);

  //
  // create FIB
  //
  fib_ = std::make_unique<Fib>(
      config_,
      Constants::kFibAgentPort,
      fibColdStartDuration,
      routeUpdatesQueue_.getReader(),
      interfaceUpdatesQueue_.getReader(),
      MonitorSubmitUrl{monitorSubmitUrl_},
      kvStore_.get(),
      context_);

  //
  // create PrefixAllocator
  //
  const auto seedPrefix =
      folly::IPAddress::createNetwork("fc00:cafe:babe::/62");
  const uint8_t allocPrefixLen = 64;
  prefixAllocator_ = std::make_unique<PrefixAllocator>(
      nodeId_,
      kvStore_.get(),
      prefixUpdatesQueue_,
      MonitorSubmitUrl{monitorSubmitUrl_},
      AllocPrefixMarker{"allocprefix:"}, // alloc_prefix_marker
      std::make_pair(seedPrefix, allocPrefixLen),
      false /* set loopback addr */,
      false /* override global address */,
      "" /* loopback interface name */,
      false /* prefix fwd type MPLS */,
      false /* prefix fwd algo KSP2_ED_ECMP */,
      Constants::kPrefixAllocatorSyncInterval,
      configStore_.get(),
      context_,
      systemPort_ /* system agent port*/);

  // Watchdog thread to monitor thread aliveness
  watchdog = std::make_unique<Watchdog>(
      nodeId_, std::chrono::seconds(1), std::chrono::seconds(60), memLimit);

  // Zmq monitor client to get counters
  zmqMonitorClient = std::make_unique<fbzmq::ZmqMonitorClient>(
      context_,
      MonitorSubmitUrl{folly::sformat("inproc://{}-monitor-submit", nodeId_)});
}

template <class Serializer>
void
OpenrWrapper<Serializer>::run() {
  try {
    // bind out publisher socket
    VLOG(2) << "Platform Publisher: Binding pub url '" << platformPubUrl_
            << "'";
    platformPubSock_.bind(fbzmq::SocketUrl{platformPubUrl_}).value();
  } catch (std::exception const& e) {
    LOG(FATAL) << "Platform Publisher: could not bind to '" << platformPubUrl_
               << "'" << folly::exceptionStr(e);
  }

  eventBase_.scheduleTimeout(std::chrono::milliseconds(100), [this]() {
    auto link = thrift::LinkEntry(
        apache::thrift::FRAGILE, "vethLMTest_" + nodeId_, 5, true, 1);

    thrift::PlatformEvent msgLink;
    msgLink.eventType = thrift::PlatformEventType::LINK_EVENT;
    msgLink.eventData = fbzmq::util::writeThriftObjStr(link, serializer_);

    // send header of event in the first 2 byte
    platformPubSock_.sendMore(
        fbzmq::Message::from(static_cast<uint16_t>(msgLink.eventType)).value());
    const auto sendNeighEntryLink =
        platformPubSock_.sendThriftObj(msgLink, serializer_);
    if (sendNeighEntryLink.hasError()) {
      LOG(ERROR) << "Error in sending PlatformEventType Entry, event Type: "
                 << folly::get_default(
                        thrift::_PlatformEventType_VALUES_TO_NAMES,
                        msgLink.eventType,
                        "UNKNOWN");
    }
  });

  // start monitor thread
  std::thread monitorThread([this]() noexcept {
    VLOG(1) << nodeId_ << " Monitor running.";
    monitor_->run();
    VLOG(1) << nodeId_ << " Monitor stopped.";
  });
  monitor_->waitUntilRunning();
  allThreads_.emplace_back(std::move(monitorThread));

  // Spawn a PrefixManager thread
  std::thread prefixManagerThread([this]() noexcept {
    VLOG(1) << nodeId_ << " PrefixManager running.";
    prefixManager_->run();
    VLOG(1) << nodeId_ << " PrefixManager stopped.";
  });
  prefixManager_->waitUntilRunning();
  allThreads_.emplace_back(std::move(prefixManagerThread));

  // Spawn a PrefixAllocator thread
  std::thread prefixAllocatorThread([this]() noexcept {
    VLOG(1) << nodeId_ << " PrefixAllocator running.";
    prefixAllocator_->run();
    VLOG(1) << nodeId_ << " PrefixAllocator stopped.";
  });
  prefixAllocator_->waitUntilRunning();
  allThreads_.emplace_back(std::move(prefixAllocatorThread));

  // start spark thread
  std::thread sparkThread([this]() {
    VLOG(1) << nodeId_ << " Spark running.";
    spark_->run();
    VLOG(1) << nodeId_ << " Spark stopped.";
  });
  spark_->waitUntilRunning();
  allThreads_.emplace_back(std::move(sparkThread));

  // start link monitor
  std::thread linkMonitorThread([this]() noexcept {
    VLOG(1) << nodeId_ << " LinkMonitor running.";
    linkMonitor_->setAsMockMode();
    linkMonitor_->run();
    VLOG(1) << nodeId_ << " LinkMonitor stopped.";
  });
  linkMonitor_->waitUntilRunning();
  allThreads_.emplace_back(std::move(linkMonitorThread));

  // start decision
  std::thread decisionThread([this]() noexcept {
    VLOG(1) << nodeId_ << " Decision running.";
    decision_->run();
    VLOG(1) << nodeId_ << " Decision stopped.";
  });
  decision_->waitUntilRunning();
  allThreads_.emplace_back(std::move(decisionThread));

  // start fib
  std::thread fibThread([this]() noexcept {
    VLOG(1) << nodeId_ << " FIB running.";
    fib_->run();
    VLOG(1) << nodeId_ << " FIB stopped.";
  });
  fib_->waitUntilRunning();
  allThreads_.emplace_back(std::move(fibThread));

  // start watchdog
  std::thread watchdogThread([this]() noexcept {
    VLOG(1) << nodeId_ << " watchdog running.";
    watchdog->run();
    VLOG(1) << nodeId_ << " watchdog stopped.";
  });
  watchdog->waitUntilRunning();
  allThreads_.emplace_back(std::move(watchdogThread));

  // start eventBase_
  allThreads_.emplace_back([&]() {
    VLOG(1) << nodeId_ << " Starting eventBase_";
    eventBase_.run();
    VLOG(1) << nodeId_ << " Stopping eventBase_";
  });
}

template <class Serializer>
void
OpenrWrapper<Serializer>::stop() {
  // Close all queues
  routeUpdatesQueue_.close();
  peerUpdatesQueue_.close();
  interfaceUpdatesQueue_.close();
  neighborUpdatesQueue_.close();
  prefixUpdatesQueue_.close();
  kvStoreUpdatesQueue_.close();
  staticRoutesQueue_.close();

  // stop all modules in reverse order
  eventBase_.stop();
  eventBase_.waitUntilStopped();
  watchdog->stop();
  watchdog->waitUntilStopped();
  fib_->stop();
  fib_->waitUntilStopped();
  decision_->stop();
  decision_->waitUntilStopped();
  linkMonitor_->stop();
  linkMonitor_->waitUntilStopped();
  spark_->stop();
  spark_->waitUntilStopped();
  prefixAllocator_->stop();
  prefixAllocator_->waitUntilStopped();
  prefixManager_->stop();
  prefixManager_->waitUntilStopped();
  monitor_->stop();
  monitor_->waitUntilStopped();
  kvStore_->stop();
  kvStore_->waitUntilStopped();
  configStore_->stop();
  configStore_->waitUntilStopped();

  // wait for all threads to finish
  for (auto& t : allThreads_) {
    t.join();
  }

  LOG(INFO) << "OpenR with nodeId: " << nodeId_ << " stopped";
}

template <class Serializer>
std::optional<thrift::IpPrefix>
OpenrWrapper<Serializer>::getIpPrefix() {
  SYNCHRONIZED(ipPrefix_) {
    if (ipPrefix_.has_value()) {
      return ipPrefix_;
    }
  }
  auto keys =
      kvStoreClient_->dumpAllWithPrefix(folly::sformat("prefix:{}", nodeId_));

  SYNCHRONIZED(ipPrefix_) {
    for (const auto& key : keys.value()) {
      auto prefixDb = fbzmq::util::readThriftObjStr<thrift::PrefixDatabase>(
          key.second.value_ref().value(), serializer_);
      if (prefixDb.deletePrefix) {
        // Skip prefixes which are about to be deleted
        continue;
      }

      for (auto& prefix : prefixDb.prefixEntries) {
        if (prefix.type == thrift::PrefixType::PREFIX_ALLOCATOR) {
          ipPrefix_ = prefix.prefix;
          break;
        }
      }
    }
  }
  return ipPrefix_.copy();
}

template <class Serializer>
bool
OpenrWrapper<Serializer>::checkKeyExists(std::string key) {
  auto keys = kvStoreClient_->dumpAllWithPrefix(key);
  return keys.has_value();
}

template <class Serializer>
bool
OpenrWrapper<Serializer>::sparkUpdateInterfaceDb(
    const std::vector<SparkInterfaceEntry>& interfaceEntries) {
  thrift::InterfaceDatabase ifDb(
      apache::thrift::FRAGILE, nodeId_, {}, thrift::PerfEvents());
  ifDb.perfEvents_ref().reset();

  for (const auto& interface : interfaceEntries) {
    ifDb.interfaces.emplace(
        interface.ifName,
        thrift::InterfaceInfo(
            apache::thrift::FRAGILE,
            true,
            interface.ifIndex,
            {}, // v4Addrs: TO BE DEPRECATED SOON
            {}, // v6LinkLocalAddrs: TO BE DEPRECATED SOON
            {toIpPrefix(interface.v4Network),
             toIpPrefix(interface.v6LinkLocalNetwork)}));
  }

  interfaceUpdatesQueue_.push(std::move(ifDb));
  return true;
}

template <class Serializer>
thrift::RouteDatabase
OpenrWrapper<Serializer>::fibDumpRouteDatabase() {
  auto routes = fib_->getRouteDb().get();
  return std::move(*routes);
}

template <class Serializer>
bool
OpenrWrapper<Serializer>::addPrefixEntries(
    const std::vector<thrift::PrefixEntry>& prefixes) {
  thrift::PrefixUpdateRequest request;
  request.cmd = thrift::PrefixUpdateCommand::ADD_PREFIXES;
  request.prefixes = prefixes;
  prefixUpdatesQueue_.push(std::move(request));
  return true;
}

template <class Serializer>
bool
OpenrWrapper<Serializer>::withdrawPrefixEntries(
    const std::vector<thrift::PrefixEntry>& prefixes) {
  thrift::PrefixUpdateRequest request;
  request.cmd = thrift::PrefixUpdateCommand::WITHDRAW_PREFIXES;
  request.prefixes = prefixes;
  prefixUpdatesQueue_.push(std::move(request));
  return true;
}

template <class Serializer>
bool
OpenrWrapper<Serializer>::checkPrefixExists(
    const thrift::IpPrefix& prefix, const thrift::RouteDatabase& routeDb) {
  for (auto const& route : routeDb.unicastRoutes) {
    if (prefix == route.dest) {
      return true;
    }
  }
  return false;
}

// define template instance for some common serializers
template class OpenrWrapper<apache::thrift::CompactSerializer>;
template class OpenrWrapper<apache::thrift::BinarySerializer>;
template class OpenrWrapper<apache::thrift::SimpleJSONSerializer>;

} // namespace openr
