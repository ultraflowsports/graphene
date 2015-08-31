/*
 * Copyright (c) 2015, Cryptonomex, Inc.
 * All rights reserved.
 *
 * This source code is provided for evaluation in private test networks only, until September 8, 2015. After this date, this license expires and
 * the code may not be used, modified or distributed for any purpose. Redistribution and use in source and binary forms, with or without modification,
 * are permitted until September 8, 2015, provided that the following conditions are met:
 *
 * 1. The code and/or derivative works are used only for private test networks consisting of no more than 10 P2P nodes.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <graphene/witness/witness.hpp>

#include <graphene/chain/database.hpp>
#include <graphene/chain/witness_object.hpp>
#include <graphene/time/time.hpp>

#include <graphene/utilities/key_conversion.hpp>

#include <fc/smart_ref_impl.hpp>
#include <fc/thread/thread.hpp>

#include <iostream>

using namespace graphene::witness_plugin;
using std::string;
using std::vector;

namespace bpo = boost::program_options;

void new_chain_banner( const graphene::chain::database& db )
{
   std::cerr << "\n"
      "********************************\n"
      "*                              *\n"
      "*   ------- NEW CHAIN ------   *\n"
      "*   - Welcome to Graphene! -   *\n"
      "*   ------------------------   *\n"
      "*                              *\n"
      "********************************\n"
      "\n";
   if( db.get_slot_at_time( graphene::time::now() ) > 200 )
   {
      std::cerr << "Your genesis seems to have an old timestamp\n"
         "Please consider using the --genesis-timestamp option to give your genesis a recent timestamp\n"
         "\n"
         ;
   }
   return;
}

void witness_plugin::plugin_set_program_options(
   boost::program_options::options_description& command_line_options,
   boost::program_options::options_description& config_file_options)
{
   auto default_priv_key = fc::ecc::private_key::regenerate(fc::sha256::hash(std::string("nathan")));
   string witness_id_example = fc::json::to_string(chain::witness_id_type(5));
   command_line_options.add_options()
         ("enable-stale-production", bpo::bool_switch()->notifier([this](bool e){_production_enabled = e;}), "Enable block production, even if the chain is stale.")
         ("required-participation", bpo::bool_switch()->notifier([this](int e){_required_witness_participation = uint32_t(e*GRAPHENE_1_PERCENT);}), "Percent of witnesses (0-99) that must be participating in order to produce blocks")
         ("allow-consecutive", bpo::bool_switch()->notifier([this](bool e){_consecutive_production_enabled = e;}), "Allow block production, even if the last block was produced by the same witness.")
         ("witness-id,w", bpo::value<vector<string>>()->composing()->multitoken(),
          ("ID of witness controlled by this node (e.g. " + witness_id_example + ", quotes are required, may specify multiple times)").c_str())
         ("private-key", bpo::value<vector<string>>()->composing()->multitoken()->
          DEFAULT_VALUE_VECTOR(std::make_pair(chain::public_key_type(default_priv_key.get_public_key()), graphene::utilities::key_to_wif(default_priv_key))),
          "Tuple of [PublicKey, WIF private key] (may specify multiple times)")
         ;
   config_file_options.add(command_line_options);
}

std::string witness_plugin::plugin_name()const
{
   return "witness";
}

void witness_plugin::plugin_initialize(const boost::program_options::variables_map& options)
{ try {
   ilog("witness plugin:  plugin_initialize() begin");
   _options = &options;
   LOAD_VALUE_SET(options, "witness-id", _witnesses, chain::witness_id_type)

   if( options.count("private-key") )
   {
      const std::vector<std::string> key_id_to_wif_pair_strings = options["private-key"].as<std::vector<std::string>>();
      for (const std::string& key_id_to_wif_pair_string : key_id_to_wif_pair_strings)
      {
         auto key_id_to_wif_pair = graphene::app::dejsonify<std::pair<chain::public_key_type, std::string> >(key_id_to_wif_pair_string);
         idump((key_id_to_wif_pair));
         fc::optional<fc::ecc::private_key> private_key = graphene::utilities::wif_to_key(key_id_to_wif_pair.second);
         if (!private_key)
         {
            // the key isn't in WIF format; see if they are still passing the old native private key format.  This is
            // just here to ease the transition, can be removed soon
            try
            {
               private_key = fc::variant(key_id_to_wif_pair.second).as<fc::ecc::private_key>();
            }
            catch (const fc::exception&)
            {
               FC_THROW("Invalid WIF-format private key ${key_string}", ("key_string", key_id_to_wif_pair.second));
            }
         }
         _private_keys[key_id_to_wif_pair.first] = *private_key;
      }
   }
   ilog("witness plugin:  plugin_initialize() end");
} FC_LOG_AND_RETHROW() }

void witness_plugin::plugin_startup()
{ try {
   ilog("witness plugin:  plugin_startup() begin");
   chain::database& d = database();
   std::set<chain::witness_id_type> bad_wits;
   //Start NTP time client
   graphene::time::now();
   for( auto wit : _witnesses )
   {
      if( d.find(wit) == nullptr )
      {
         if( app().is_finished_syncing() )
         {
            elog("ERROR: Unable to find witness ${w}, even though syncing has finished. This witness will be ignored.",
                 ("w", wit));
            continue;
         } else {
            wlog("WARNING: Unable to find witness ${w}. Postponing initialization until syncing finishes.",
                 ("w", wit));
            app().syncing_finished.connect([this]{plugin_startup();});
            return;
         }
      }

      auto signing_key = wit(d).signing_key;
      if( !_private_keys.count(signing_key) )
      {
         // Check if it's a duplicate key of one I do have
         bool found_duplicate = false;
         for( const auto& private_key : _private_keys )
            if( chain::public_key_type(private_key.second.get_public_key()) == signing_key )
            {
               ilog("Found duplicate key: ${k1} matches ${k2}; using this key to sign for ${w}",
                    ("k1", private_key.first)("k2", signing_key)("w", wit));
               _private_keys[signing_key] = private_key.second;
               found_duplicate = true;
               break;
            }
         if( found_duplicate )
            continue;

         elog("Unable to find key for witness ${w}. Removing it from my witnesses.", ("w", wit));
         bad_wits.insert(wit);
      }
   }
   for( auto wit : bad_wits )
      _witnesses.erase(wit);

   if( !_witnesses.empty() )
   {
      ilog("Launching block production for ${n} witnesses.", ("n", _witnesses.size()));
      app().set_block_production(true);
      if( _production_enabled && (d.head_block_num() == 0) )
         new_chain_banner(d);
      schedule_production_loop();
   } else
      elog("No witnesses configured! Please add witness IDs and private keys to configuration.");
   ilog("witness plugin:  plugin_startup() end");
} FC_CAPTURE_AND_RETHROW() }

void witness_plugin::plugin_shutdown()
{
   graphene::time::shutdown_ntp_time();
   return;
}

void witness_plugin::schedule_production_loop()
{
   //Schedule for the next second's tick regardless of chain state
   // If we would wait less than 200ms, wait for the whole second.
   fc::time_point now = graphene::time::now();
   fc::time_point_sec next_second( now + fc::microseconds( 1200000 ) );
   wdump( (now.time_since_epoch().count())(next_second) );
   _block_production_task = fc::schedule([this]{block_production_loop();},
                                         next_second, "Witness Block Production");
}

block_production_condition::block_production_condition_enum witness_plugin::block_production_loop()
{
   block_production_condition::block_production_condition_enum result;
   fc::mutable_variant_object capture;
   try
   {
      result = maybe_produce_block(capture);
   }
   catch( const fc::canceled_exception& )
   {
      //We're trying to exit. Go ahead and let this one out.
      throw;
   }
   catch( const fc::exception& e )
   {
      elog("Got exception while generating block:\n${e}", ("e", e.to_detail_string()));
      result = block_production_condition::exception_producing_block;
   }

   switch( result )
   {
      case block_production_condition::produced:
         ilog("Generated block #${n} with timestamp ${t} at time ${c}", (capture));
         break;
      case block_production_condition::not_synced:
         ilog("Not producing block because production is disabled until we receive a recent block (see: --enable-stale-production)");
         break;
      case block_production_condition::not_my_turn:
         ilog("Not producing block because it isn't my turn");
         break;
      case block_production_condition::not_time_yet:
         ilog("Not producing block because slot has not yet arrived");
         break;
      case block_production_condition::no_private_key:
         ilog("Not producing block because I don't have the private key for ${scheduled_key}", (capture) );
         break;
      case block_production_condition::low_participation:
         elog("Not producing block because node appears to be on a minority fork with only ${pct}% witness participation", (capture) );
         break;
      case block_production_condition::lag:
         elog("Not producing block because node didn't wake up within 500ms of the slot time.");
         break;
      case block_production_condition::consecutive:
         elog("Not producing block because the last block was generated by the same witness.\nThis node is probably disconnected from the network so block production has been disabled.\nDisable this check with --allow-consecutive option.");
         break;
      case block_production_condition::exception_producing_block:
         break;
   }

   schedule_production_loop();
   return result;
}

block_production_condition::block_production_condition_enum witness_plugin::maybe_produce_block( fc::mutable_variant_object& capture )
{
   chain::database& db = database();
   fc::time_point now_fine = graphene::time::now();
   fc::time_point_sec now = now_fine + fc::microseconds( 500000 );

   // If the next block production opportunity is in the present or future, we're synced.
   if( !_production_enabled )
   {
      if( db.get_slot_time(1) >= now )
         _production_enabled = true;
      else
         return block_production_condition::not_synced;
   }

   // is anyone scheduled to produce now or one second in the future?
   uint32_t slot = db.get_slot_at_time( now );
   if( slot == 0 )
   {
      capture("next_time", db.get_slot_time(1));
      return block_production_condition::not_time_yet;
   }

   //
   // this assert should not fail, because now <= db.head_block_time()
   // should have resulted in slot == 0.
   //
   // if this assert triggers, there is a serious bug in get_slot_at_time()
   // which would result in allowing a later block to have a timestamp
   // less than or equal to the previous block
   //
   assert( now > db.head_block_time() );

   graphene::chain::witness_id_type scheduled_witness = db.get_scheduled_witness( slot );
   // we must control the witness scheduled to produce the next block.
   if( _witnesses.find( scheduled_witness ) == _witnesses.end() )
   {
      capture("scheduled_witness", scheduled_witness);
      return block_production_condition::not_my_turn;
   }

   fc::time_point_sec scheduled_time = db.get_slot_time( slot );
   graphene::chain::public_key_type scheduled_key = scheduled_witness( db ).signing_key;
   auto private_key_itr = _private_keys.find( scheduled_key );

   if( private_key_itr == _private_keys.end() )
   {
      capture("scheduled_key", scheduled_key);
      return block_production_condition::no_private_key;
   }

   uint32_t prate = db.witness_participation_rate();
   if( prate < _required_witness_participation )
   {
      capture("pct", uint32_t(100*uint64_t(prate) / GRAPHENE_1_PERCENT));
      return block_production_condition::low_participation;
   }

   if( llabs((scheduled_time - now).count()) > fc::milliseconds( 500 ).count() )
   {
      capture("scheduled_time", scheduled_time)("now", now);
      return block_production_condition::lag;
   }

   if( !_consecutive_production_enabled )
   {
      if( db.get_dynamic_global_properties().current_witness == scheduled_witness )
      {
         capture("scheduled_witness", scheduled_witness);
         return block_production_condition::consecutive;
      }
   }

   auto block = db.generate_block(
      scheduled_time,
      scheduled_witness,
      private_key_itr->second,
      graphene::chain::database::skip_nothing
      );
   capture("n", block.block_num())("t", block.timestamp)("c", now);
   p2p_node().broadcast(net::block_message(block));

   return block_production_condition::produced;
}
