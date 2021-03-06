#include <functional>
#include <string>
#include <stdexcept>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <sstream>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/info_parser.hpp>

#include <prime_server/prime_server.hpp>
#include <prime_server/http_protocol.hpp>
using namespace prime_server;

#include <valhalla/midgard/logging.h>
#include <valhalla/baldr/json.h>
#include <valhalla/proto/tripdirections.pb.h>
#include <valhalla/proto/directions_options.pb.h>
#include <valhalla/odin/util.h>

#include "tyr/service.h"

using namespace valhalla;
using namespace valhalla::baldr;
using namespace valhalla::odin;
using namespace valhalla::tyr;
using namespace std;

namespace {

  namespace osrm_serializers {
    /*
    OSRM output looks like this:
    {
        "hint_data": {
            "locations": [
                "_____38_SADaFQQAKwEAABEAAAAAAAAAdgAAAFfLwga4tW0C4P6W-wAARAA",
                "fzhIAP____8wFAQA1AAAAC8BAAAAAAAAAAAAAP____9Uu20CGAiX-wAAAAA"
            ],
            "checksum": 2875622111
        },
        "route_name": [ "West 26th Street", "Madison Avenue" ],
        "via_indices": [ 0, 9 ],
        "found_alternative": false,
        "route_summary": {
            "end_point": "West 29th Street",
            "start_point": "West 26th Street",
            "total_time": 145,
            "total_distance": 878
        },
        "via_points": [ [ 40.744377, -73.990433 ], [40.745811, -73.988075 ] ],
        "route_instructions": [
            [ "10", "West 26th Street", 216, 0, 52, "215m", "SE", 118 ],
            [ "1", "East 26th Street", 153, 2, 29, "153m", "SE", 120 ],
            [ "7", "Madison Avenue", 237, 3, 25, "236m", "NE", 29 ],
            [ "7", "East 29th Street", 155, 6, 29, "154m", "NW", 299 ],
            [ "1", "West 29th Street", 118, 7, 21, "117m", "NW", 299 ],
            [ "15", "", 0, 8, 0, "0m", "N", 0 ]
        ],
        "route_geometry": "ozyulA~p_clCfc@ywApTar@li@ybBqe@c[ue@e[ue@i[ci@dcB}^rkA",
        "status_message": "Found route between points",
        "status": 0
    }
    */

    json::ArrayPtr route_name(const valhalla::odin::TripDirections& trip_directions){
      auto route_name = json::array({});
      if(trip_directions.maneuver_size() > 0) {
        if(trip_directions.maneuver(0).street_name_size() > 0) {
          route_name->push_back(trip_directions.maneuver(0).street_name(0));
        }
        if(trip_directions.maneuver(trip_directions.maneuver_size() - 1).street_name_size() > 0) {
          route_name->push_back(trip_directions.maneuver(trip_directions.maneuver_size() - 1).street_name(0));
        }
      }
      return route_name;
    }

    json::ArrayPtr via_indices(const valhalla::odin::TripDirections& trip_directions){
      auto via_indices = json::array({});
      if(trip_directions.maneuver_size() > 0) {
        via_indices->push_back(static_cast<uint64_t>(0));
        via_indices->push_back(static_cast<uint64_t>(trip_directions.maneuver_size() - 1));
      }
      return via_indices;
    }

    json::MapPtr route_summary(const valhalla::odin::TripDirections& trip_directions){
      auto route_summary = json::map({});
      if(trip_directions.maneuver_size() > 0) {
        if(trip_directions.maneuver(0).street_name_size() > 0)
          route_summary->emplace("start_point", trip_directions.maneuver(0).street_name(0));
        else
          route_summary->emplace("start_point", string(""));
        if(trip_directions.maneuver(trip_directions.maneuver_size() - 1).street_name_size() > 0)
          route_summary->emplace("end_point", trip_directions.maneuver(trip_directions.maneuver_size() - 1).street_name(0));
        else
          route_summary->emplace("end_point", string(""));
      }
      uint64_t seconds = 0, meters = 0;
      for(const auto& maneuver : trip_directions.maneuver()) {
        meters += static_cast<uint64_t>(maneuver.length() * 1000.f);
        seconds += static_cast<uint64_t>(maneuver.time());
      }
      route_summary->emplace("total_time", seconds);
      route_summary->emplace("total_distance", meters);
      return route_summary;
    }

    json::ArrayPtr via_points(const valhalla::odin::TripDirections& trip_directions){
      auto via_points = json::array({});
      for(const auto& location : trip_directions.location()) {
        via_points->emplace_back(json::array({json::fp_t{location.ll().lat(),6}, json::fp_t{location.ll().lng(),6}}));
      }
      return via_points;
    }

    const std::unordered_map<int, std::string> maneuver_type = {
        { static_cast<int>(valhalla::odin::TripDirections_Maneuver_Type_kNone),             "0" },//NoTurn = 0,
        { static_cast<int>(valhalla::odin::TripDirections_Maneuver_Type_kContinue),         "1" },//GoStraight,
        { static_cast<int>(valhalla::odin::TripDirections_Maneuver_Type_kBecomes),          "1" },//GoStraight,
        { static_cast<int>(valhalla::odin::TripDirections_Maneuver_Type_kRampStraight),     "1" },//GoStraight,
        { static_cast<int>(valhalla::odin::TripDirections_Maneuver_Type_kStayStraight),     "1" },//GoStraight,
        { static_cast<int>(valhalla::odin::TripDirections_Maneuver_Type_kMerge),            "1" },//GoStraight,
        { static_cast<int>(valhalla::odin::TripDirections_Maneuver_Type_kFerryEnter),       "1" },//GoStraight,
        { static_cast<int>(valhalla::odin::TripDirections_Maneuver_Type_kFerryExit),        "1" },//GoStraight,
        { static_cast<int>(valhalla::odin::TripDirections_Maneuver_Type_kSlightRight),      "2" },//TurnSlightRight,
        { static_cast<int>(valhalla::odin::TripDirections_Maneuver_Type_kRight),            "3" },//TurnRight,
        { static_cast<int>(valhalla::odin::TripDirections_Maneuver_Type_kRampRight),        "3" },//TurnRight,
        { static_cast<int>(valhalla::odin::TripDirections_Maneuver_Type_kExitRight),        "3" },//TurnRight,
        { static_cast<int>(valhalla::odin::TripDirections_Maneuver_Type_kStayRight),        "3" },//TurnRight,
        { static_cast<int>(valhalla::odin::TripDirections_Maneuver_Type_kSharpRight),       "4" },//TurnSharpRight,
        { static_cast<int>(valhalla::odin::TripDirections_Maneuver_Type_kUturnLeft),        "5" },//UTurn,
        { static_cast<int>(valhalla::odin::TripDirections_Maneuver_Type_kUturnRight),       "5" },//UTurn,
        { static_cast<int>(valhalla::odin::TripDirections_Maneuver_Type_kSharpLeft),        "6" },//TurnSharpLeft,
        { static_cast<int>(valhalla::odin::TripDirections_Maneuver_Type_kLeft),             "7" },//TurnLeft,
        { static_cast<int>(valhalla::odin::TripDirections_Maneuver_Type_kRampLeft),         "7" },//TurnLeft,
        { static_cast<int>(valhalla::odin::TripDirections_Maneuver_Type_kExitLeft),         "7" },//TurnLeft,
        { static_cast<int>(valhalla::odin::TripDirections_Maneuver_Type_kStayLeft),         "7" },//TurnLeft,
        { static_cast<int>(valhalla::odin::TripDirections_Maneuver_Type_kSlightLeft),       "8" },//TurnSlightLeft,
        //{ static_cast<int>(valhalla::odin::TripDirections_Maneuver_Type_k),               "9" },//ReachViaLocation,
        { static_cast<int>(valhalla::odin::TripDirections_Maneuver_Type_kRoundaboutEnter),  "11" },//EnterRoundAbout,
        { static_cast<int>(valhalla::odin::TripDirections_Maneuver_Type_kRoundaboutExit),   "12" },//LeaveRoundAbout,
        //{ static_cast<int>(valhalla::odin::TripDirections_Maneuver_Type_k),               "13" },//StayOnRoundAbout,
        { static_cast<int>(valhalla::odin::TripDirections_Maneuver_Type_kStart),            "14" },//StartAtEndOfStreet,
        { static_cast<int>(valhalla::odin::TripDirections_Maneuver_Type_kStartRight),       "14" },//StartAtEndOfStreet,
        { static_cast<int>(valhalla::odin::TripDirections_Maneuver_Type_kStartLeft),        "14" },//StartAtEndOfStreet,
        { static_cast<int>(valhalla::odin::TripDirections_Maneuver_Type_kDestination),      "15" },//ReachedYourDestination,
        { static_cast<int>(valhalla::odin::TripDirections_Maneuver_Type_kDestinationRight), "15" },//ReachedYourDestination,
        { static_cast<int>(valhalla::odin::TripDirections_Maneuver_Type_kDestinationLeft),  "15" },//ReachedYourDestination,
        //{ static_cast<int>valhalla::odin::TripDirections_Maneuver_Type_k),                "16" },//EnterAgainstAllowedDirection,
        //{ static_cast<int>valhalla::odin::TripDirections_Maneuver_Type_k),                "17" },//LeaveAgainstAllowedDirection
    };

    const std::unordered_map<int, std::string> cardinal_direction_string = {
      { static_cast<int>(valhalla::odin::TripDirections_Maneuver_CardinalDirection_kNorth),     "N" },
      { static_cast<int>(valhalla::odin::TripDirections_Maneuver_CardinalDirection_kNorthEast), "NE" },
      { static_cast<int>(valhalla::odin::TripDirections_Maneuver_CardinalDirection_kEast),      "E" },
      { static_cast<int>(valhalla::odin::TripDirections_Maneuver_CardinalDirection_kSouthEast), "SE" },
      { static_cast<int>(valhalla::odin::TripDirections_Maneuver_CardinalDirection_kSouth),     "S" },
      { static_cast<int>(valhalla::odin::TripDirections_Maneuver_CardinalDirection_kSouthWest), "SW" },
      { static_cast<int>(valhalla::odin::TripDirections_Maneuver_CardinalDirection_kWest),      "W" },
      { static_cast<int>(valhalla::odin::TripDirections_Maneuver_CardinalDirection_kNorthWest), "NW" }
    };

    json::ArrayPtr route_instructions(const valhalla::odin::TripDirections& trip_directions){
      auto route_instructions = json::array({});
      for(const auto& maneuver : trip_directions.maneuver()) {
        //if we dont know the type of maneuver then skip it
        auto maneuver_text = maneuver_type.find(static_cast<int>(maneuver.type()));
        if(maneuver_text == maneuver_type.end())
          continue;

        //length
        std::ostringstream length;
        length << static_cast<uint64_t>(maneuver.length()*1000.f) << "m";

        //json
        route_instructions->emplace_back(json::array({
          maneuver_text->second, //maneuver type
          (maneuver.street_name_size() ? maneuver.street_name(0) : string("")), //street name
          static_cast<uint64_t>(maneuver.length() * 1000.f), //length in meters
          static_cast<uint64_t>(maneuver.begin_shape_index()), //index in the shape
          static_cast<uint64_t>(maneuver.time()), //time in seconds
          length.str(), //length as a string with a unit suffix
          cardinal_direction_string.find(static_cast<int>(maneuver.begin_cardinal_direction()))->second, // one of: N S E W NW NE SW SE
          static_cast<uint64_t>(maneuver.begin_heading())
        }));
      }
      return route_instructions;
    }

    void serialize(const valhalla::odin::DirectionsOptions& directions_options,
      const valhalla::odin::TripDirections& trip_directions,
      std::ostringstream& stream) {

      //TODO: worry about multipoint routes


      //build up the json object
      auto json = json::map
      ({
        {"hint_data", json::map
          ({
            {"locations", json::array({ string(""), string("") })}, //TODO: are these internal ids?
            {"checksum", static_cast<uint64_t>(0)} //TODO: what is this exactly?
          })
        },
        {"route_name", route_name(trip_directions)}, //TODO: list of all of the streets or just the via points?
        {"via_indices", via_indices(trip_directions)}, //maneuver index
        {"found_alternative", static_cast<bool>(false)}, //no alt route support
        {"route_summary", route_summary(trip_directions)}, //start/end name, total time/distance
        {"via_points", via_points(trip_directions)}, //array of lat,lng pairs
        {"route_instructions", route_instructions(trip_directions)}, //array of maneuvers
        {"route_geometry", trip_directions.shape()}, //polyline encoded shape
        {"status_message", string("Found route between points")}, //found route between points OR cannot find route between points
        {"status", static_cast<uint64_t>(0)} //0 success or 207 no route
      });

      //serialize it
      stream << *json;
    }
  }

  namespace valhalla_serializers {
    /*
    valhalla output looks like this:
    {
        "trip":
    {
        "status": 0,
        "locations": [
           {
            "longitude": -76.4791,
            "latitude": 40.4136,
             "stopType": 0
           },
           {
            "longitude": -76.5352,
            "latitude": 40.4029,
            "stopType": 0
           }
         ],
        "units": "kilometers"
        "summary":
    {
        "distance": 4973,
        "time": 325
    },
    "legs":
    [
      {
          "summary":
      {
          "distance": 4973,
          "time": 325
      },
      "maneuvers":
      [
        {
            "beginShapeIndex": 0,
            "distance": 633,
            "writtenInstruction": "Start out going west on West Market Street.",
            "streetNames":
            [
                "West Market Street"
            ],
            "type": 1,
            "time": 41
        },
        {
            "beginShapeIndex": 7,
            "distance": 4340,
            "writtenInstruction": "Continue onto Jonestown Road.",
            "streetNames":
            [
                "Jonestown Road"
            ],
            "type": 8,
            "time": 284
        },
        {
            "beginShapeIndex": 40,
            "distance": 0,
            "writtenInstruction": "You have arrived at your destination.",
            "type": 4,
            "time": 0
        }
    ],
    "shape": "gysalAlg|zpC~Clt@tDtx@hHfaBdKl{BrKbnApGro@tJrz@jBbQj@zVt@lTjFnnCrBz}BmFnoB]pHwCvm@eJxtATvXTnfAk@|^z@rGxGre@nTpnBhBbQvXduCrUr`Edd@naEja@~gAhk@nzBxf@byAfm@tuCvDtOvNzi@|jCvkKngAl`HlI|}@`N`{Adx@pjE??xB|J"
    }
    ],
    "status_message": "Found route between points"
    }
    }
    */
    using namespace std;

    json::MapPtr summary(const valhalla::odin::TripDirections& trip_directions){

      // TODO: multiple legs.

      auto route_summary = json::map({});
      route_summary->emplace("time", static_cast<uint64_t>(trip_directions.summary().time()));
      route_summary->emplace("length", json::fp_t{trip_directions.summary().length(), 3});
      return route_summary;
    }

    json::ArrayPtr locations(const valhalla::odin::TripDirections& trip_directions){
      auto locations = json::array({});
      for(const auto& location : trip_directions.location()) {

        auto loc = json::map({});

        if (location.type() == valhalla::odin::TripDirections_Location_Type_kThrough) {
          loc->emplace("type", std::string("through"));
        } else {
          loc->emplace("type", std::string("break"));
        }
        loc->emplace("lat", json::fp_t{location.ll().lat(), 6});
        loc->emplace("lon",json::fp_t{location.ll().lng(), 6});
        if (!location.name().empty())
          loc->emplace("name",location.name());
        if (!location.street().empty())
          loc->emplace("street",location.street());
        if (!location.city().empty())
          loc->emplace("city",location.city());
        if (!location.state().empty())
          loc->emplace("state",location.state());
        if (!location.postal_code().empty())
          loc->emplace("postal_code",location.postal_code());
        if (!location.country().empty())
          loc->emplace("country",location.country());
        if (location.has_heading())
          loc->emplace("heading",static_cast<uint64_t>(location.heading()));
        if (!location.date_time().empty())
          loc->emplace("date_time",location.date_time());

        //loc->emplace("sideOfStreet",location.side_of_street());

        locations->emplace_back(loc);
      }

      return locations;
    }


    json::ArrayPtr legs(const valhalla::odin::TripDirections& trip_directions){

      // TODO: multiple legs.
      auto legs = json::array({});
      auto leg = json::map({});
      auto summary = json::map({});
      auto maneuvers = json::array({});

      for(const auto& maneuver : trip_directions.maneuver()) {

        auto man = json::map({});

        man->emplace("type", static_cast<uint64_t>(maneuver.type()));
        man->emplace("instruction", maneuver.text_instruction());
        //“verbalTransitionAlertInstruction” : “<verbalTransitionAlertInstruction>”,
        //“verbalPreTransitionInstruction” : “<verbalPreTransitionInstruction>”,
        //“verbalPostTransitionInstruction” : “<verbalPostTransitionInstruction>”,
        auto street_names = json::array({});

        for (int i = 0; i < maneuver.street_name_size(); i++)
          street_names->emplace_back(maneuver.street_name(i));

        if (street_names->size())
          man->emplace("street_names", street_names);
        man->emplace("time", static_cast<uint64_t>(maneuver.time()));
        man->emplace("length", json::fp_t{maneuver.length(), 3});
        man->emplace("begin_shape_index", static_cast<uint64_t>(maneuver.begin_shape_index()));
        man->emplace("end_shape_index", static_cast<uint64_t>(maneuver.end_shape_index()));

        if (maneuver.portions_toll())
          man->emplace("toll", maneuver.portions_toll());
        if (maneuver.portions_unpaved())
          man->emplace("rough", maneuver.portions_unpaved());

        //  man->emplace("hasGate", maneuver.);
        //  man->emplace("hasFerry", maneuver.);
        //“portionsTollNote” : “<portionsTollNote>”,
        //“portionsUnpavedNote” : “<portionsUnpavedNote>”,
        //“gateAccessRequiredNote” : “<gateAccessRequiredNote>”,
        //“checkFerryInfoNote” : “<checkFerryInfoNote>”
        maneuvers->emplace_back(man);

      }
      leg->emplace("maneuvers", maneuvers);
      summary->emplace("time", static_cast<uint64_t>(trip_directions.summary().time()));
      summary->emplace("length", json::fp_t{trip_directions.summary().length(), 3});
      leg->emplace("summary",summary);
      leg->emplace("shape", trip_directions.shape());

      legs->emplace_back(leg);
      return legs;
    }

    void serialize(const valhalla::odin::DirectionsOptions& directions_options,
                   const valhalla::odin::TripDirections& trip_directions,
                   std::ostringstream& stream) {

      //TODO: worry about multipoint routes

      //build up the json object
      auto json = json::map
          ({
          {"trip", json::map
          ({
              {"locations", locations(trip_directions)},
              {"summary", summary(trip_directions)},
              {"legs", legs(trip_directions)},
              {"status_message", string("Found route between points")}, //found route between points OR cannot find route between points
              {"status", static_cast<uint64_t>(0)}, //0 success or 207 no route
              {"units", std::string((directions_options.units() == valhalla::odin::DirectionsOptions::kKilometers) ? "kilometers" : "miles")}
          })
        }
      });

      //serialize it
      stream << *json;
    }
  }

  //TODO: throw this in the header to make it testable?
  class tyr_worker_t {
   public:
    tyr_worker_t(const boost::property_tree::ptree& config):config(config) {
    }
    worker_t::result_t work(const std::list<zmq::message_t>& job, void* request_info) {
      auto& info = *static_cast<http_request_t::info_t*>(request_info);
      LOG_INFO("Got Tyr Request " + std::to_string(info.id));
      try{
        //get some info about what we need to do
        std::string request_str(static_cast<const char*>(job.front().data()), job.front().size());
        std::stringstream stream(request_str);
        boost::property_tree::ptree request;
        boost::property_tree::read_info(stream, request);

        //see if we can get some options
        valhalla::odin::DirectionsOptions directions_options;
        auto options = request.get_child_optional("directions_options");
        if(options)
          directions_options = valhalla::odin::GetDirectionsOptions(*options);

        //crack open the directions
        odin::TripDirections trip_directions;
        trip_directions.ParseFromArray(job.back().data(), static_cast<int>(job.back().size()));

        //jsonp callback if need be
        std::ostringstream json_stream;
        auto jsonp = request.get_optional<std::string>("jsonp");
        if(jsonp)
          json_stream << *jsonp << '(';
        if(request.get_optional<std::string>("osrm"))
          osrm_serializers::serialize(directions_options, trip_directions, json_stream);
        else
          valhalla_serializers::serialize(directions_options, trip_directions, json_stream);
        if(jsonp)
          json_stream << ')';

        worker_t::result_t result{false};
        http_response_t response(200, "OK", json_stream.str(), headers_t{{"Content-type", "application/json;charset=utf-8"}});
        response.from_info(info);
        result.messages.emplace_back(response.to_string());
        return result;
      }
      catch(const std::exception& e) {
        worker_t::result_t result{false};
        http_response_t response(400, "Bad Request", e.what());
        response.from_info(info);
        result.messages.emplace_back(response.to_string());
        return result;
      }
    }
   protected:
    boost::property_tree::ptree config;
  };
}

namespace valhalla {
  namespace tyr {
    void run_service(const boost::property_tree::ptree& config) {
      //gets requests from thor proxy
      auto upstream_endpoint = config.get<std::string>("tyr.service.proxy") + "_out";
      //sends them on to odin
      //auto downstream_endpoint = config.get<std::string>("tyr.service.proxy_multi") + "_in";
      //or returns just location information back to the server
      auto loopback_endpoint = config.get<std::string>("httpd.service.loopback");

      //listen for requests
      zmq::context_t context;
      prime_server::worker_t worker(context, upstream_endpoint, "ipc://NO_ENDPOINT", loopback_endpoint,
        std::bind(&tyr_worker_t::work, tyr_worker_t(config), std::placeholders::_1, std::placeholders::_2));
      worker.work();

      //TODO: should we listen for SIGINT and terminate gracefully/exit(0)?
    }
  }
}
