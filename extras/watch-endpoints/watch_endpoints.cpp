// watch-endpoints: the Workbench's bridge to Windows MIDI Services.
//
// Lists the UMP endpoints Windows MIDI Services offers and keeps a
// connection open to each, one JSON object per line:
//
//    out  {"c":"m","m":text}                      a message for the log
//    out  {"c":"a","i":id,"n":name,"g":[...],"usbDetails":{...}}
//                                                 an endpoint was added
//    out  {"c":"r","i":id}                        an endpoint was removed
//    out  {"c":"u","i":id,"u":[words]}            a packet was received
//    in   {"a":"u","i":id,"u":[words]}            send a packet
//
// Built against the Windows MIDI Services SDK (Windows.Devices.Midi2), whose
// DLL ships beside this program. The helper the Workbench bundled before was
// built against a release candidate of the SDK that Microsoft has withdrawn;
// with the current service it still lists endpoints but its connections
// carry nothing in either direction.

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Midi2.h>
#include <winrt/Windows.Devices.Midi2.Enumeration.h>

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <map>
#include <mutex>
#include <regex>
#include <string>
#include <vector>

using namespace winrt::Windows::Devices::Midi2;
using namespace winrt::Windows::Devices::Midi2::Enumeration;
namespace foundation = winrt::Windows::Foundation;

namespace
{
   std::mutex out_lock;

   void put(std::string const& line)
   {
      std::lock_guard<std::mutex> guard{out_lock};
      std::fwrite(line.data(), 1, line.size(), stdout);
      std::fputc('\n', stdout);
      std::fflush(stdout);
   }

   std::string json_string(std::string const& s)
   {
      std::string r = "\"";
      for (unsigned char c : s)
      {
         if (c == '"' || c == '\\')
         {
            r += '\\';
            r += char(c);
         }
         else if (c < 0x20)
         {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            r += buf;
         }
         else
         {
            r += char(c);
         }
      }
      return r + "\"";
   }

   std::string utf8(winrt::hstring const& s)
   {
      return winrt::to_string(s);
   }

   // "\\?\swd#midisrv#midiu_ks_123_outpin.0_inpin.2#{guid}" becomes
   // "SWD_MIDISRV_MIDIU_KS_123_OUTPIN.0_INPIN.2", the form the earlier
   // helper used, so the Workbench's saved settings still match.
   std::string short_id(winrt::hstring const& endpoint_id)
   {
      std::string s = utf8(endpoint_id);
      if (s.rfind("\\\\?\\", 0) == 0)
         s = s.substr(4);
      if (auto brace = s.find("#{"); brace != std::string::npos)
         s = s.substr(0, brace);
      for (auto& c : s)
      {
         if (c == '#')
            c = '_';
         else if (c >= 'a' && c <= 'z')
            c = char(c - 'a' + 'A');
      }
      return s;
   }

   struct endpoint
   {
      winrt::hstring             device_id;
      MidiEndpointConnection     connection{nullptr};
   };

   std::mutex endpoints_lock;
   std::map<std::string, endpoint> endpoints;

   std::string describe(MidiEndpointDeviceInformation const& info)
   {
      std::string line = "{\"c\":\"a\",\"i\":" + json_string(short_id(info.EndpointDeviceId()))
         + ",\"n\":" + json_string(utf8(info.Name()));

      auto blocks = info.GetGroupTerminalBlocks();
      if (blocks.Size() != 0)
      {
         line += ",\"g\":[";
         bool first = true;
         for (auto const& gb : blocks)
         {
            if (!first)
               line += ",";
            first = false;
            line += "{\"direction\":" + std::to_string(int(gb.Direction()))
               + ",\"firstGroup\":" + std::to_string(int(gb.FirstGroup().Index()))
               + ",\"gbIdx\":" + std::to_string(int(gb.Number()))
               + ",\"name\":" + json_string(utf8(gb.Name()))
               + ",\"numberGroups\":" + std::to_string(int(gb.GroupCount()))
               + ",\"protocol\":" + std::to_string(int(gb.Protocol())) + "}";
         }
         line += "]";
      }

      auto transport = info.GetTransportSuppliedInfo();
      std::string serial = utf8(transport.SerialNumber());
      if (auto parent = info.GetParentDeviceInformation(); parent != nullptr)
      {
         if (serial.empty())
            serial = utf8(parent.UsbSerialNumber());
      }
      line += ",\"usbDetails\":{\"iManufacturer\":" + json_string(utf8(transport.ManufacturerName()))
         + ",\"iProduct\":" + json_string(utf8(transport.Name()))
         + ",\"iSerialNumber\":" + json_string(serial) + "}}";
      return line;
   }

   void open(MidiSession const& session, MidiEndpointDeviceInformation const& info)
   {
      auto const id = short_id(info.EndpointDeviceId());
      auto connection = session.CreateEndpointConnection(info.EndpointDeviceId());

      connection.MessageReceived(
         [id](IMidiMessageReceivedEventSource const&, MidiMessageReceivedEventArgs const& args)
         {
            uint32_t w0 = 0, w1 = 0, w2 = 0, w3 = 0;
            auto const count = args.FillWords(w0, w1, w2, w3);
            uint32_t const words[4] = {w0, w1, w2, w3};
            std::string line = "{\"c\":\"u\",\"i\":" + json_string(id) + ",\"u\":[";
            for (uint8_t i = 0; i != count; ++i)
            {
               if (i != 0)
                  line += ",";
               line += std::to_string(words[i]);
            }
            put(line + "]}");
         });

      if (!connection.Open())
      {
         put("{\"c\":\"m\",\"m\":" + json_string("Could not open " + id) + "}");
         return;
      }

      {
         std::lock_guard<std::mutex> guard{endpoints_lock};
         endpoints[id] = endpoint{info.EndpointDeviceId(), connection};
      }
      put(describe(info));
   }

   void send(std::string const& id, std::vector<uint32_t> const& words)
   {
      MidiEndpointConnection connection{nullptr};
      {
         std::lock_guard<std::mutex> guard{endpoints_lock};
         auto it = endpoints.find(id);
         if (it == endpoints.end())
            return;
         connection = it->second.connection;
      }

      auto const now = MidiClock::TimestampConstantSendImmediately();
      switch (words.size())
      {
         case 1: connection.SendSingleMessageWords(now, words[0]); break;
         case 2: connection.SendSingleMessageWords(now, words[0], words[1]); break;
         case 3: connection.SendSingleMessageWords(now, words[0], words[1], words[2]); break;
         case 4: connection.SendSingleMessageWords(now, words[0], words[1], words[2], words[3]); break;
         default: break;
      }
   }
}

int main()
{
   winrt::init_apartment();

   if (!MidiApi::EnsureServiceAvailable())
   {
      put("{\"c\":\"m\",\"m\":\"Could not start the MIDI Service\"}");
      return 1;
   }
   put("{\"c\":\"m\",\"m\":\"Verified that the MIDI Service is available and started\"}");

   auto session = MidiSession::Create(L"MIDI 2.0 Workbench");

   auto const filter =
      MidiEndpointDeviceInformationFilters::StandardNativeMidi1ByteFormat |
      MidiEndpointDeviceInformationFilters::StandardNativeUniversalMidiPacketFormat |
      MidiEndpointDeviceInformationFilters::DiagnosticLoopback;

   auto watcher = MidiEndpointDeviceWatcher::Create(filter);

   watcher.Added(
      [&](MidiEndpointDeviceWatcher const&, MidiEndpointDeviceInformationAddedEventArgs const& args)
      {
         try
         {
            open(session, args.AddedDevice());
         }
         catch (winrt::hresult_error const& e)
         {
            put("{\"c\":\"m\",\"m\":" + json_string(utf8(e.message())) + "}");
         }
      });

   watcher.Removed(
      [&](MidiEndpointDeviceWatcher const&, MidiEndpointDeviceInformationRemovedEventArgs const& args)
      {
         auto const id = short_id(args.RemovedDevice().EndpointDeviceId());
         {
            std::lock_guard<std::mutex> guard{endpoints_lock};
            endpoints.erase(id);
         }
         put("{\"c\":\"r\",\"i\":" + json_string(id) + "}");
      });

   watcher.Start();

   // One request per line, as the Workbench writes them.
   std::regex const request{R"re("a"\s*:\s*"u".*"i"\s*:\s*"([^"]*)".*"u"\s*:\s*\[([^\]]*)\])re"};
   std::string line;
   while (std::getline(std::cin, line))
   {
      std::smatch m;
      if (!std::regex_search(line, m, request))
         continue;

      std::vector<uint32_t> words;
      std::string const list = m[2].str();
      std::size_t pos = 0;
      while (pos < list.size())
      {
         auto const end = list.find(',', pos);
         auto const item = list.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
         if (!item.empty())
            words.push_back(uint32_t(std::stoull(item)));
         if (end == std::string::npos)
            break;
         pos = end + 1;
      }

      try
      {
         send(m[1].str(), words);
      }
      catch (winrt::hresult_error const& e)
      {
         put("{\"c\":\"m\",\"m\":" + json_string(utf8(e.message())) + "}");
      }
   }

   watcher.Stop();
   return 0;
}
