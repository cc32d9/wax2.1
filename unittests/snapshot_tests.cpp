#include <sstream>

#include <eosio/chain/block_log.hpp>
#include <eosio/chain/global_property_object.hpp>
#include <eosio/chain/snapshot.hpp>
#include <eosio/testing/tester.hpp>

#include <boost/mpl/list.hpp>
#include <boost/test/unit_test.hpp>

#include <contracts.hpp>
#include <snapshots.hpp>

using namespace eosio;
using namespace testing;
using namespace chain;

chainbase::bfs::path get_parent_path(chainbase::bfs::path blocks_dir, int ordinal) {
   chainbase::bfs::path leaf_dir = blocks_dir.filename();
   if (leaf_dir.generic_string() == std::string("blocks")) {
      blocks_dir = blocks_dir.parent_path();
      leaf_dir = blocks_dir.filename();
      try {
         auto ordinal_for_config = boost::lexical_cast<int>(leaf_dir.generic_string());
         blocks_dir = blocks_dir.parent_path();
      }
      catch(const boost::bad_lexical_cast& ) {
         // no extra ordinal directory added to path
      }
   }
   return blocks_dir / std::to_string(ordinal);
}

controller::config copy_config(const controller::config& config, int ordinal) {
   controller::config copied_config = config;
   auto parent_path = get_parent_path(config.blocks_dir, ordinal);
   copied_config.blocks_dir = parent_path / config.blocks_dir.filename().generic_string();
   copied_config.state_dir = parent_path / config.state_dir.filename().generic_string();
   return copied_config;
}

controller::config copy_config_and_files(const controller::config& config, int ordinal) {
   controller::config copied_config = copy_config(config, ordinal);
   fc::create_directories(copied_config.blocks_dir);
   fc::copy(config.blocks_dir / "blocks.log", copied_config.blocks_dir / "blocks.log");
   fc::copy(config.blocks_dir / config::reversible_blocks_dir_name, copied_config.blocks_dir / config::reversible_blocks_dir_name );
   return copied_config;
}

class snapshotted_tester : public base_tester {
public:
   enum config_file_handling { dont_copy_config_files, copy_config_files };
   snapshotted_tester(controller::config config, const snapshot_reader_ptr& snapshot, int ordinal,
           config_file_handling copy_files_from_config = config_file_handling::dont_copy_config_files) {
      FC_ASSERT(config.blocks_dir.filename().generic_string() != "."
                && config.state_dir.filename().generic_string() != ".", "invalid path names in controller::config");

      controller::config copied_config = (copy_files_from_config == copy_config_files)
                                         ? copy_config_and_files(config, ordinal) : copy_config(config, ordinal);

      init(copied_config, snapshot);
   }

   signed_block_ptr produce_block( fc::microseconds skip_time = fc::milliseconds(config::block_interval_ms) )override {
      return _produce_block(skip_time, false);
   }

   signed_block_ptr produce_empty_block( fc::microseconds skip_time = fc::milliseconds(config::block_interval_ms) )override {
      control->abort_block();
      return _produce_block(skip_time, true);
   }

   signed_block_ptr finish_block()override {
      return _finish_block();
   }

   bool validate() { return true; }
};

struct variant_snapshot_suite {
   using writer_t = variant_snapshot_writer;
   using reader_t = variant_snapshot_reader;
   using write_storage_t = fc::mutable_variant_object;
   using snapshot_t = fc::variant;

   struct writer : public writer_t {
      writer( const std::shared_ptr<write_storage_t>& storage )
      :writer_t(*storage)
      ,storage(storage)
      {

      }

      std::shared_ptr<write_storage_t> storage;
   };

   struct reader : public reader_t {
      explicit reader(const snapshot_t& storage)
      :reader_t(storage)
      {}
   };


   static auto get_writer() {
      return std::make_shared<writer>(std::make_shared<write_storage_t>());
   }

   static auto finalize(const std::shared_ptr<writer>& w) {
      w->finalize();
      return snapshot_t(*w->storage);
   }

   static auto get_reader( const snapshot_t& buffer) {
      return std::make_shared<reader>(buffer);
   }

   template<typename Snapshot>
   static void save_tmpfile(const snapshot_t &in) {
     std::ofstream dst;
     dst.open(Snapshot::tmpname_json(), std::ofstream::out | std::ofstream::binary);
     fc::json::to_stream(dst, in);
     dst.close();
     std::cout << "Wrote " << Snapshot::tmpname_json() << "\n";
   }
};

struct buffered_snapshot_suite {
   using writer_t = ostream_snapshot_writer;
   using reader_t = istream_snapshot_reader;
   using write_storage_t = std::ostringstream;
   using snapshot_t = std::string;
   using read_storage_t = std::istringstream;

   struct writer : public writer_t {
      writer( const std::shared_ptr<write_storage_t>& storage )
      :writer_t(*storage)
      ,storage(storage)
      {

      }

      std::shared_ptr<write_storage_t> storage;
   };

   struct reader : public reader_t {
      explicit reader(const std::shared_ptr<read_storage_t>& storage)
      :reader_t(*storage)
      ,storage(storage)
      {}

      std::shared_ptr<read_storage_t> storage;
   };


   static auto get_writer() {
      return std::make_shared<writer>(std::make_shared<write_storage_t>());
   }

   static auto finalize(const std::shared_ptr<writer>& w) {
      w->finalize();
      return w->storage->str();
   }

   static auto get_reader( const snapshot_t& buffer) {
      return std::make_shared<reader>(std::make_shared<read_storage_t>(buffer));
   }

   template<typename Snapshot>
   static void save_tmpfile(const snapshot_t &in) {
     std::ofstream dst;
     dst.open(Snapshot::tmpname_bin(), std::ofstream::out | std::ofstream::binary);
     dst << in;
     dst.close();
     std::cout << "Wrote " << Snapshot::tmpname_bin() << "\n";
   }
};

BOOST_AUTO_TEST_SUITE(snapshot_tests)

using snapshot_suites = boost::mpl::list<variant_snapshot_suite, buffered_snapshot_suite>;

namespace {
   void variant_diff_helper(const fc::variant& lhs, const fc::variant& rhs, std::function<void(const std::string&, const fc::variant&, const fc::variant&)>&& out){
      if (lhs.get_type() != rhs.get_type()) {
         out("", lhs, rhs);
      } else if (lhs.is_object() ) {
         const auto& l_obj = lhs.get_object();
         const auto& r_obj = rhs.get_object();
         static const std::string sep = ".";

         // test keys from LHS
         std::set<std::string_view> keys;
         for (const auto& entry: l_obj) {
            const auto& l_val = entry.value();
            const auto& r_iter = r_obj.find(entry.key());
            if (r_iter == r_obj.end()) {
               out(sep + entry.key(), l_val, fc::variant());
            } else {
               const auto& r_val = r_iter->value();
               variant_diff_helper(l_val, r_val, [&out, &entry](const std::string& path, const fc::variant& lhs, const fc::variant& rhs){
                  out(sep + entry.key() + path, lhs, rhs);
               });
            }

            keys.insert(entry.key());
         }

         // print keys in RHS that were not tested
         for (const auto& entry: r_obj) {
            if (keys.find(entry.key()) != keys.end()) {
               continue;
            }
            const auto& r_val = entry.value();
            out(sep + entry.key(), fc::variant(), r_val);
         }
      } else if (lhs.is_array()) {
         const auto& l_arr = lhs.get_array();
         const auto& r_arr = rhs.get_array();

         // diff common
         auto common_count = std::min(l_arr.size(), r_arr.size());
         for (size_t idx = 0; idx < common_count; idx++) {
            const auto& l_val = l_arr.at(idx);
            const auto& r_val = r_arr.at(idx);
            variant_diff_helper(l_val, r_val, [&](const std::string& path, const fc::variant& lhs, const fc::variant& rhs){
               out( std::string("[") + std::to_string(idx) + std::string("]") + path, lhs, rhs);
            });
         }

         // print lhs additions
         for (size_t idx = common_count; idx < lhs.size(); idx++) {
            const auto& l_val = l_arr.at(idx);
            out( std::string("[") + std::to_string(idx) + std::string("]"), l_val, fc::variant());
         }

         // print rhs additions
         for (size_t idx = common_count; idx < rhs.size(); idx++) {
            const auto& r_val = r_arr.at(idx);
            out( std::string("[") + std::to_string(idx) + std::string("]"), fc::variant(), r_val);
         }

      } else if (!(lhs == rhs)) {
         out("", lhs, rhs);
      }
   }

   void print_variant_diff(const fc::variant& lhs, const fc::variant& rhs) {
      variant_diff_helper(lhs, rhs, [](const std::string& path, const fc::variant& lhs, const fc::variant& rhs){
         std::cout << path << std::endl;
         if (!lhs.is_null()) {
            std::cout << " < " << fc::json::to_pretty_string(lhs) << std::endl;
         }

         if (!rhs.is_null()) {
            std::cout << " > " << fc::json::to_pretty_string(rhs) << std::endl;
         }
      });
   }

   template <typename SNAPSHOT_SUITE>
   void verify_integrity_hash(const controller& lhs, const controller& rhs) {
      const auto lhs_integrity_hash = lhs.calculate_integrity_hash();
      const auto rhs_integrity_hash = rhs.calculate_integrity_hash();
      if (std::is_same_v<SNAPSHOT_SUITE, variant_snapshot_suite> && lhs_integrity_hash.str() != rhs_integrity_hash.str()) {
         auto lhs_latest_writer = SNAPSHOT_SUITE::get_writer();
         lhs.write_snapshot(lhs_latest_writer);
         auto lhs_latest = SNAPSHOT_SUITE::finalize(lhs_latest_writer);

         auto rhs_latest_writer = SNAPSHOT_SUITE::get_writer();
         rhs.write_snapshot(rhs_latest_writer);
         auto rhs_latest = SNAPSHOT_SUITE::finalize(rhs_latest_writer);

         print_variant_diff(lhs_latest, rhs_latest);
      }
      BOOST_REQUIRE_EQUAL(lhs_integrity_hash.str(), rhs_integrity_hash.str());
   }
}


BOOST_AUTO_TEST_CASE_TEMPLATE(test_compatible_versions, SNAPSHOT_SUITE, snapshot_suites)
{
   tester chain(setup_policy::preactivate_feature_and_new_bios);

   ///< Begin deterministic code to generate blockchain for comparison
   // TODO: create a utility that will write new bin/json gzipped files based on this
   chain.create_account(N(snapshot));
   chain.produce_blocks(1);
   chain.set_code(N(snapshot), contracts::snapshot_test_wasm());
   chain.set_abi(N(snapshot), contracts::snapshot_test_abi().data());
   chain.produce_blocks(1);
   chain.control->abort_block();

   {
     // write a temporary snapshot file
      auto tmp_writer = SNAPSHOT_SUITE::get_writer();
      chain.control->write_snapshot(tmp_writer);
      auto snpsht = SNAPSHOT_SUITE::finalize(tmp_writer);
      SNAPSHOT_SUITE::template save_tmpfile<snapshots::snap_v2>(snpsht);
   }
}

BOOST_AUTO_TEST_CASE_TEMPLATE(test_pending_schedule_snapshot, SNAPSHOT_SUITE, snapshot_suites)
{
   tester chain(setup_policy::preactivate_feature_and_new_bios);
   auto block = chain.produce_block();
   BOOST_REQUIRE_EQUAL(block->block_num(), 3); // ensure that test setup stays consistent with original snapshot setup
   chain.create_account(N(snapshot));
   block = chain.produce_block();
   BOOST_REQUIRE_EQUAL(block->block_num(), 4);

   auto res = chain.set_producers( {N(snapshot)} );
   block = chain.produce_block();
   BOOST_REQUIRE_EQUAL(block->block_num(), 5);
   chain.control->abort_block();
   ///< End deterministic code to generate blockchain for comparison

   {
     // write a temporary snapshot file
      auto tmp_writer = SNAPSHOT_SUITE::get_writer();
      chain.control->write_snapshot(tmp_writer);
      auto snpsht = SNAPSHOT_SUITE::finalize(tmp_writer);
      SNAPSHOT_SUITE::template save_tmpfile<snapshots::snap_v2_prod_sched>(snpsht);
   }

}


BOOST_AUTO_TEST_SUITE_END()
