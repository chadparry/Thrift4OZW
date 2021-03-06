/*
Thrift4OZW - An Apache Thrift wrapper for OpenZWave
----------------------------------------------------
Copyright (c) 2011 Elias Karakoulakis <elias.karakoulakis@gmail.com>

SOFTWARE NOTICE AND LICENSE

Thrift4OZW is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published
by the Free Software Foundation, either version 3 of the License,
or (at your option) any later version.

Thrift4OZW is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with Thrift4OZW.  If not, see <http://www.gnu.org/licenses/>.

for more information on the LGPL, see:
http://en.wikipedia.org/wiki/GNU_Lesser_General_Public_License
*/

// 
// Main.cpp: OpenZWave Thrift Server for Project Ansible
// (c) 2011 Elias Karakoulakis <elias.karakoulakis@gmail.com>
//

#include "RemoteManager.h"
#include <protocol/TJSONProtocol.h>
#include <server/TSimpleServer.h>
#include <transport/TServerSocket.h>
#include <transport/TBufferTransports.h>

#include <string>
#include <sstream>
#include <iostream>
#include "unistd.h"
// we're using Boost's program_options
#include <boost/program_options.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>
// also we're using boost's filesystem classes
#include <boost/filesystem.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/exception/info.hpp>

typedef boost::error_info<struct tag_stack_str,std::string> stack_info;

namespace po = boost::program_options;
namespace fs = boost::filesystem;

using namespace ::apache::thrift;
using namespace ::apache::thrift::protocol;
using namespace ::apache::thrift::transport;
using namespace ::apache::thrift::server;

using boost::shared_ptr;
 
using namespace OpenZWave;

// boost: extra includes
#include <boost/thread.hpp>
// the global mutex for accessing OpenZWave library
static boost::recursive_mutex     g_criticalSection;

//
// Struct to hold all valid OpenZWave ValueID's
// (used by RemoteManager_server.cpp)
typedef struct
{
	uint32			m_homeId;
	uint8			m_nodeId;
	bool			m_polled;
    //std::map<uint64, ValueID*>	m_values;
    list<ValueID>	m_values;
} NodeInfo;
//
static list<NodeInfo*>     g_nodes;

// OpenZWave includes
#include "Manager.h"
#include "Notification.h"
#include "Log.h"

static uint32 g_homeId = 0;
static bool   g_initFailed = false;
//

static boost::condition_variable  initCond ;
static boost::mutex               initMutex;

// Stomp client
#include "BoostStomp.hpp"
static STOMP::BoostStomp* stomp_client;
static string*          notifications_topic = new string("/topic/zwave/monitor");

// JSON body indicator
static bool jsonMessageBody = false;

// Show OZW+BoostStomp debug messages?
static bool debugMsg = false;

//-----------------------------------------------------------------------------
// <GetNodeInfo>
// Callback that is triggered when a value, group or node changes
//-----------------------------------------------------------------------------
NodeInfo* GetNodeInfo
(
	Notification const* _notification
)
{
	uint32 const homeId = _notification->GetHomeId();
	uint8 const nodeId = _notification->GetNodeId();
	for( list<NodeInfo*>::iterator it = g_nodes.begin(); it != g_nodes.end(); ++it )
	{
		NodeInfo* nodeInfo = *it;
		if( ( nodeInfo->m_homeId == homeId ) && ( nodeInfo->m_nodeId == nodeId ) )
		{
			return nodeInfo;
		}
	}

	return NULL;
}

//-----------------------------------------------------------------------------
// <jsonifyHeaders>
// Represent headers as JSON object
//-----------------------------------------------------------------------------
string jsonifyHeaders(STOMP::hdrmap& headers)
{
	string hdrjson="";	
	STOMP::hdrmap::iterator it;
	hdrjson.append("{");
	for(it=headers.begin(); it!=headers.end(); ++it)
	{
	  hdrjson.append("\"");
	  hdrjson.append(it->first);
	  hdrjson.append("\":\"");
	  hdrjson.append(it->second);
	  hdrjson.append("\"");
	  hdrjson.append(",");
	}
	//pop_back introduced in C++0x
	//http://stackoverflow.com/questions/2310939/remove-last-character-from-c-string 	
	//hdrjson.pop_back();
	hdrjson.resize (hdrjson.size () - 1);
	hdrjson.append("}");
	return hdrjson;
}

//-----------------------------------------------------------------------------
// <OnNotification>
// Callback that is triggered by OpenZWave when a value, group or node changes
//-----------------------------------------------------------------------------
void OnNotification
(
    Notification const* _notification,
    void* _context
)
{
    bool notify_stomp = true;
    bool send_valueID = false;
    
    // Must do this inside a critical section to avoid conflicts with the main thread
    g_criticalSection.lock();
    
    switch( _notification->GetType() )
    {
        /**< A new node value has been added to OpenZWave's list. 
        These notifications occur after a node has been discovered, 
        and details of its command classes have been received.  
        Each command class may generate one or more values, 
        depending on the complexity of the item being represented.  */
        case Notification::Type_ValueAdded: 
        {
            if( NodeInfo* nodeInfo = GetNodeInfo( _notification ) )
            {
                // Add the new value to the node's value list
                ValueID v = _notification->GetValueID();
                nodeInfo->m_values.push_back( v );
            }
            send_valueID = true;
            break;
        }

        /**< A node value has been removed from OpenZWave's list.  
        This only occurs when a node is removed. */
        case Notification::Type_ValueRemoved:
        {
            if( NodeInfo* nodeInfo = GetNodeInfo( _notification ) )
            {
                // Remove the value from out list
				for( list<ValueID>::iterator it = nodeInfo->m_values.begin(); it != nodeInfo->m_values.end(); ++it )
				{
					if( (*it) == _notification->GetValueID() )
					{
						nodeInfo->m_values.erase( it );
						break;
					}
				}

                //~ // Remove the value from out list
                for( list<ValueID>::iterator it = nodeInfo->m_values.begin(); it != nodeInfo->m_values.end(); ++it )
                {
                    if( (*it) == _notification->GetValueID() )
                    {
                        nodeInfo->m_values.erase( it );
                        break;
                    }
                }
            }
            send_valueID = true;
            break;
        }
            
        /**< A node value has been updated from the Z-Wave network. */
        case Notification::Type_ValueChanged:
        case Notification::Type_ValueRefreshed: {
            send_valueID = true;
        /**< The associations for the node have changed. The application 
        should rebuild any group information it holds about the node. */
        }
        break;

        case Notification::Type_Group:
        /**< A new node has been found (not already stored in zwcfg*.xml file) */
        case Notification::Type_NodeNew:
        /**< Basic node information has been receievd, such as whether the node is a 
        listening device, a routing device and its baud rate and basic, generic and 
        specific types. It is after this notification that you can call Manager::GetNodeType 
        to obtain a label containing the device description. */
        case Notification::Type_NodeProtocolInfo:
        /**< A node has triggered an event.  This is commonly caused when a node sends 
        a Basic_Set command to the controller.  The event value is stored in the notification. */
/*        case Notification::Type_NodeEvent:
        {
            if( NodeInfo* nodeInfo = GetNodeInfo( _notification ) )
            {
                // One of the node's association groups has changed
                // TBD...
            }
            break;
        }
*/

        /**< A new node has been added to OpenZWave's list.  This may be due 
        to a device being added to the Z-Wave network, or because 
        the application is initializing itself. */
        case Notification::Type_NodeAdded:
        {
            // Add the new node to our list
            NodeInfo* nodeInfo = new NodeInfo();
            nodeInfo->m_homeId = _notification->GetHomeId();
            nodeInfo->m_nodeId = _notification->GetNodeId();
            nodeInfo->m_polled = false;		
            g_nodes.push_back( nodeInfo );
            break;
        }

        /**< A node has been removed from OpenZWave's list.  This may be due 
        to a device being removed from the Z-Wave network, or because the application is closing. */
        case Notification::Type_NodeRemoved:
        {
            // Remove the node from our list
            uint32 const homeId = _notification->GetHomeId();
            uint8 const nodeId = _notification->GetNodeId();
            for( list<NodeInfo*>::iterator it = g_nodes.begin(); it != g_nodes.end(); ++it )
            {
                NodeInfo* nodeInfo = *it;
                if( ( nodeInfo->m_homeId == homeId ) && ( nodeInfo->m_nodeId == nodeId ) )
                {
                    g_nodes.erase( it );
                    break;
                }
            }
            break;
        }

        // Type_NodeNaming missing

        /**< Polling of a node has been successfully turned off by a call to Manager::DisablePoll */
        case Notification::Type_PollingDisabled:
        {
            if( NodeInfo* nodeInfo = GetNodeInfo( _notification ) )
            {
                nodeInfo->m_polled = false;
            }
            break;
        }

        /**< Polling of a node has been successfully turned on by a call to Manager::EnablePoll */
        case Notification::Type_PollingEnabled:
        {
            if( NodeInfo* nodeInfo = GetNodeInfo( _notification ) )
            {
                nodeInfo->m_polled = true;
            }
            break;
        }
        
        /**< A driver for a PC Z-Wave controller has been added and is ready to use.  The notification 
        will contain the controller's Home ID, which is needed to call most of the Manager methods. */
        case Notification::Type_DriverReady:
        {
            g_homeId = _notification->GetHomeId();
            initCond.notify_all();
            break;
        }

        /**< Driver failed to load */
        case Notification::Type_DriverFailed:
        {
            g_initFailed = true;
            initCond.notify_all();
            break;
        }
        
        //missing Type_DriverReset:  < All nodes and values for this driver have been removed.  This is sent instead of potentially hundreds of individual node and value notifications. */
        //missing MsgComplete  < The last message that was sent is now complete. */
        //missing Type_EssentialNodeQueriesComplete,	/**< The queries on a node that are essential to its operation have been completed. The node can now handle incoming messages. */
        //missing Type_NodeQueriesComplete,			/**< All the initialisation queries on a node have been completed. */
        
        /*< All awake nodes have been queried, so client application can expected complete data for these nodes. */
        case Notification::Type_AwakeNodesQueried:        
        /**< All nodes have been queried, so client application can expected complete data. */
        case Notification::Type_AllNodesQueried:
        {
                //initCond.notify_all();
                break;
        }

        case Notification::Type_AllNodesQueriedSomeDead:
        {
            // TODO: mark dead nodes for deletion?
                //initCond.notify_all();
                break;
        }
            
        default:
        	break;
    }
    
    // now we can send the captured event to STOMP queue
    //
    if (notify_stomp) {
        STOMP::hdrmap headers;
        headers["NotificationNodeId"] = to_string<uint16_t>(_notification->GetNodeId(), std::hex);
        headers["NotificationType"] =  to_string<uint32_t>(_notification->GetType(), std::hex);
        headers["NotificationByte"] =  to_string<uint16_t>(_notification->GetByte(), std::hex);
        if (send_valueID) {
            headers["HomeID"] =  to_string<uint32_t>(_notification->GetValueID().GetHomeId(), std::hex);
            headers["ValueID"] =  to_string<uint64_t>(_notification->GetValueID().GetId(), std::hex);
        }
        //
        string body = jsonMessageBody ? jsonifyHeaders(headers) : "";
        //
        stomp_client->send(*notifications_topic, headers, body);
    }
    //
    g_criticalSection.unlock();
}

// Send all known values via STOMP
void send_all_values() {
    //
    g_criticalSection.lock();
    //
    for( list<NodeInfo*>::iterator node_it = g_nodes.begin(); node_it != g_nodes.end(); ++node_it )
	{
		NodeInfo* nodeInfo = *node_it;
        
		for( list<ValueID>::iterator val_iter = nodeInfo->m_values.begin(); val_iter != nodeInfo->m_values.end(); ++val_iter )
		{
			ValueID v = *val_iter;
            STOMP::hdrmap headers;
            headers["HomeID"] =  to_string<uint32_t>(v.GetHomeId(), std::hex);
            headers["ValueID"] =  to_string<uint64_t>(v.GetId(), std::hex);
            //
            string response = jsonMessageBody ? jsonifyHeaders(headers) : "";
            stomp_client->send(*notifications_topic, headers,response );         
		}
	}
	//
	g_criticalSection.unlock();
}

// the Thrift-generated (and manually patched) RemoteManager implementation
// for OpenZWave::Manager class
#include "gen-cpp/RemoteManager_server.cpp"
//

void dump_trace(exception& e, const char* context) {
        cerr << "------------------------------------------------------------------------" << endl;
        cerr << context << ": " << e.what() << endl;
        if ( std::string const *stack = boost::get_error_info<stack_info>(e) ) {                    
            cerr << "Stack Trace:" << endl << stack << endl;
        }
        cerr << "------------------------------------------------------------------------" << endl;
}

// -----------------------------------------
int main(int argc, char *argv[]) {
// -----------------------------------------
	string  stomp_host, ozw_port, ozw_conf, ozw_user;
    int     stomp_port, thrift_port;
    // 
    string ozwconf_default = "/usr/share/openzwave/config";
    //
    fs::path current_dir = fs::path(get_current_dir_name());
    fs::path ozw_config_dir = fs::exists(ozwconf_default) ? 
        fs::path(ozwconf_default) :
        current_dir.parent_path() / "open-zwave-read-only/config/";
    //
    try {        
        // Declare the supported options.
        po::options_description desc("----------------------------------------\nProject Ansible - OpenZWave orbiter\n----------------------------------------\ncommand-line arguments");
        desc.add_options()
            ("help,?", "print this help message")
            ("stomphost,h",   po::value<string>(&stomp_host)->default_value("localhost"), "STOMP server hostname")
            ("stompport,s",   po::value<int>(&stomp_port)->default_value(61613), "STOMP server port number")
            ("thriftport,t",  po::value<int>(&thrift_port)->default_value(9090), "our Thrift service port")
            ("ozwconf,c",     po::value<string>(&ozw_conf)->default_value(ozw_config_dir.string()), "OpenZWave's manufacturer database")
            ("ozwuser,u",     po::value<string>(&ozw_user)->default_value(current_dir.string()), "OpenZWave's user config database")
            ("ozwport,p",     po::value<string>(&ozw_port)->default_value("/dev/ttyUSB0"), "OpenZWave's driver dongle")
            ("json,j",        po::bool_switch(&jsonMessageBody), "Should stomp messages have JSON body?")
            ("debug,d",       po::bool_switch(&debugMsg), "Show debug logging from OpenZwave and BoostStomp?")
        ;
        // a boost:program_options variable map
        po::variables_map vm;        
        po::parsed_options l_parsed = po::parse_command_line(argc, argv, desc);
        po::store(l_parsed, vm);
        po::notify(vm);
        // exit on help        
        if (vm.count("help")) {
            cout << desc << "\n";
            return 1;
        }
    }
    catch (exception& e) 
    {
        dump_trace(e, "parsing command-line options");
        return 2;
    }

    // ------------------
    try {
        cout << "Connecting to STOMP server at " << stomp_host << ":" << stomp_port << std::endl;
        // connect to STOMP server in order to send openzwave notifications 
        stomp_client = new STOMP::BoostStomp(stomp_host, stomp_port);
        stomp_client->start();
        stomp_client->enable_debug_msgs(debugMsg);
        cout << "Connected to STOMP server." << std::endl;
    } 
    catch (exception& e) 
    {
        dump_trace(e, "connecting to STOMP");
        return 3;
    } 

    // OpenZWave daemon initialization
    try {    
        // Create the OpenZWave Manager.
        // The first argument is the path to the config files (where the manufacturer_specific.xml file is located
        // The second argument is the path for saved Z-Wave network state and the log file.  If you leave it NULL 
        // the log file will appear in the program's working directory.
        cout << "Initializing OpenZWave" << endl;
        cout << "    configuration    : " << ozw_conf << std::endl;
        cout << "    user preferences : " << ozw_user << std::endl;
        cout << "    controller port  : " << ozw_port << std::endl;
        cout.flush();
        
        Options::Create(ozw_conf, ozw_user, "" );
        Options::Get()->Lock();
    
        Manager::Create();
          
        // Add a callback handler to the manager. 
        Manager::Get()->AddWatcher( OnNotification, NULL );
    
        // Add a Z-Wave Driver
        Manager::Get()->AddDriver( ozw_port );
        
        // Set OpenZwave's log level
        LogLevel ll_save  = debugMsg ? LogLevel_Debug : LogLevel_None;
        LogLevel ll_queue = debugMsg ? LogLevel_Debug : LogLevel_Detail;
        LogLevel ll_dump  = debugMsg ? LogLevel_Warning : LogLevel_Error;
        Log::SetLoggingState(ll_save, ll_queue, ll_dump);
        
    } 
    catch (exception& e) 
    {
        dump_trace(e, "initializing OpenZWave");
        return 4;
    };
    
    // Thrift server initialization
    TSimpleServer* server;
    try {   
        // THRIFT: initialize RemoteManager
        shared_ptr<RemoteManagerHandler> handler(new RemoteManagerHandler());
        shared_ptr<TProcessor> processor(new RemoteManagerProcessor(handler));
        TServerSocket* ss = new TServerSocket(thrift_port);
        shared_ptr<TServerTransport> serverTransport(ss);
        shared_ptr<TTransportFactory> transportFactory(new TBufferedTransportFactory());
        shared_ptr<TProtocolFactory> protocolFactory(new TBinaryProtocolFactory());
        server = new TSimpleServer(processor, serverTransport, transportFactory, protocolFactory);
    }
    catch (exception& e) 
    {
        dump_trace(e, "setting up Thrift SimpleServer");
        return 5;
    }
    
    try {
        // Now we just wait for the driver to become ready, and then write out the loaded config.
        boost::unique_lock<boost::mutex> initLock(initMutex);
        initCond.wait(initLock);
        usleep(500);
    }
    catch (exception& e) 
    {
        dump_trace(e, "waiting for OpenZWave to complete initialization");
        return 6;
    }

    cout << "------------------------------------------------------------------------" << endl;
    cout << "OpenZWave orbiter is now active, Thrift interface listening on port " << thrift_port << endl;
    cout << "------------------------------------------------------------------------" << endl;
    cout.flush();
    
    // ready to serve!
    try {
        server->serve();
    }    
    catch (exception& e) 
    {
        dump_trace(e, "server.serve()");
        return 7;
    }
    
    return 0;
}
