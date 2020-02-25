#include <appbase/application.hpp>

#include <eosio/http_plugin/http_plugin.hpp>
#include <eosio/wallet_plugin/wallet_plugin.hpp>
#include <eosio/wallet_api_plugin/wallet_api_plugin.hpp>
#include <eosio/version/version.hpp>

#include <fc/log/logger_config.hpp>
#include <fc/exception/exception.hpp>

#include <boost/exception/diagnostic_information.hpp>
#include <signal.h>

#include <pwd.h>
#include "config.hpp"

using namespace appbase;
using namespace eosio;

bfs::path determine_home_directory()
{
   bfs::path home;
   struct passwd* pwd = getpwuid(getuid());
   if(pwd) {
      home = pwd->pw_dir;
   }
   else {
      home = getenv("HOME");
   }
   if(home.empty())
      home = "./";
   return home;
}

int get_parent_pid(int argc, char** argv) {
   size_t idx = 1;
   for (; idx < argc; ++idx)
   {
      if (!strcmp(argv[idx], "--parent_pid"))
      {
         break;
      }
   }
   const size_t pid_idx = idx + 1;
   if (pid_idx < argc)
   {
      std::cout << "Parent id:" << atoi(argv[pid_idx]) << std::endl;
      return atoi(argv[pid_idx]);
   }
   return -1;
}

int main(int argc, char** argv)
{
   try {
      app().set_version_string(eosio::version::version_client());
      app().set_full_version_string(eosio::version::version_full());
      bfs::path home = determine_home_directory();
      app().set_default_data_dir(home / "eosio-wallet");
      app().set_default_config_dir(home / "eosio-wallet");
      http_plugin::set_defaults({
         .default_unix_socket_path = keosd::config::key_store_executable_name + ".sock",
         .default_http_port = 0
      });
      const int parent_id = get_parent_pid(argc, argv);
      int argc_new = (parent_id > 0) ? argc - 2 : argc;
      char** argv_new = (parent_id > 0) ? &argv[2] : argv;
         std::cout << "keosd main.cpp --------11111111--------\n";
      app().register_plugin<wallet_api_plugin>();
      if(!app().initialize<wallet_plugin, wallet_api_plugin, http_plugin>(argc_new, argv_new))
         return -1;
      std::cout << "keosd main.cpp --------22222222--------\n";
      auto& http = app().get_plugin<http_plugin>();
      std::cout << "keosd main.cpp --------3333333--------\n";
      http.add_handler("/v1/" + keosd::config::key_store_executable_name + "/stop", [&a=app()](string, string, url_response_callback cb) { cb(200, fc::variant(fc::variant_object())); a.quit(); } );
      std::cout << "keosd main.cpp --------4444444--------\n";
      app().startup();
      std::cout << "keosd main.cpp --------5555555--------\n";

      if (parent_id > 0)
      {
         const int ret_sig_que = kill(parent_id, SIGUSR1);
         std::cout << "Send signal to parent with ret code:" << ret_sig_que << std::endl;
      }

      app().exec();
      std::cout << "keosd main.cpp --------6666666--------\n";
   } catch (const fc::exception& e) {
      elog("${e}", ("e",e.to_detail_string()));
   } catch (const boost::exception& e) {
      elog("${e}", ("e",boost::diagnostic_information(e)));
   } catch (const std::exception& e) {
      elog("${e}", ("e",e.what()));
   } catch (...) {
      elog("unknown exception");
   }
   return 0;
}
