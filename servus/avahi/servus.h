/* Copyright (c) 2014-2015, Stefan.Eilemann@epfl.ch
 *               2015, Juan Hernando <jhernando@fi.upm.es>
 *
 * This file is part of Servus <https://github.com/HBPVIS/Servus>
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License version 3.0 as published
 * by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-client/publish.h>
#include <avahi-common/error.h>
#include <avahi-common/simple-watch.h>

#include <net/if.h>
#include <arpa/inet.h>
#include <stdexcept>

#include <cassert>

#ifdef SERVUS_USE_CXX03
#  include <boost/chrono.hpp>
#  include <boost/thread/mutex.hpp>
    using boost::mutex;
    typedef boost::unique_lock< mutex > ScopedLock;
    namespace chrono = boost::chrono;
#else
#  include <mutex>
#include <avahi-common/address.h>

using std::mutex;
    typedef std::unique_lock< mutex > ScopedLock;
    namespace chrono = std::chrono;
#endif

#define WARN std::cerr << __FILE__ << ":" << __LINE__ << ": "

// http://stackoverflow.com/questions/14430906
//   Proper way of doing this is using the threaded polling in avahi
namespace
{
static mutex _mutex;

int64_t _elapsedMilliseconds(
    const chrono::high_resolution_clock::time_point& startTime )
{
    const chrono::high_resolution_clock::time_point& endTime =
        chrono::high_resolution_clock::now();
    return chrono::nanoseconds( endTime - startTime ).count() / 1000000;
}

}

namespace servus
{
namespace avahi
{
namespace
{
AvahiSimplePoll* _newSimplePoll()
{
    ScopedLock lock( _mutex );
    return avahi_simple_poll_new();
}
}

class Servus : public detail::Servus
{
public:
    explicit Servus( const std::string& name )
        : detail::Servus( name )
        , _poll( _newSimplePoll( ))
        , _client( 0 )
        , _browser( 0 )
        , _group( 0 )
        , _result( servus::Servus::Result::PENDING )
        , _port( 0 )
        , _announcable( false )
        , _scope( servus::Servus::IF_ALL )
    {
        if( !_poll )
            throw std::runtime_error( "Can't setup avahi poll device" );

        int error = 0;
        ScopedLock lock( _mutex );
        _client = avahi_client_new( avahi_simple_poll_get( _poll ),
                                    (AvahiClientFlags)(0), _clientCBS, this,
                                    &error );
        if( !_client )
            throw std::runtime_error(
                std::string( "Can't setup avahi client: " ) +
                avahi_strerror( error ));
    }

    virtual ~Servus()
    {
        withdraw();
        endBrowsing();

        ScopedLock lock( _mutex );
        if( _client )
            avahi_client_free( _client );
        if( _poll )
            avahi_simple_poll_free( _poll );
    }

    std::string getClassName() const { return "avahi"; }

    servus::Servus::Result announce(const servus::Servus::Interface interface_, const unsigned short port,
                                     const std::string& instance ) final
    {

        ScopedLock lock( _mutex );

        _result = servus::Servus::Result::PENDING;
        _port = port;
        _interface = interface_;
        if( instance.empty( ))
            _announce = getHostname();
        else
            _announce = instance;

        if( _announcable )
            _createServices();
        else
        {
            const chrono::high_resolution_clock::time_point& startTime =
                chrono::high_resolution_clock::now();
            while( !_announcable &&
                   _result == servus::Servus::Result::PENDING &&
                   _elapsedMilliseconds( startTime ) < ANNOUNCE_TIMEOUT )
            {
                avahi_simple_poll_iterate( _poll, ANNOUNCE_TIMEOUT );
            }
        }

        return servus::Servus::Result( _result );
    }

    void withdraw() final
    {
        ScopedLock lock( _mutex );
        _announce.clear();
        _port = 0;
        if( _group )
            avahi_entry_group_reset( _group );
    }

    bool isAnnounced() const final
    {
        ScopedLock lock( _mutex );
        return ( _group && !avahi_entry_group_is_empty( _group ));
    }

    servus::Servus::Result beginBrowsing(
        const ::servus::Servus::Interface addr ) final
    {
        if( _browser )
            return servus::Servus::Result( servus::Servus::Result::PENDING );

        ScopedLock lock( _mutex );
        _scope = addr;
        _instanceMap.clear();
        _result = servus::Servus::Result::SUCCESS;
        _browser = avahi_service_browser_new( _client, AVAHI_IF_UNSPEC,
                                              AVAHI_PROTO_UNSPEC, _name.c_str(),
                                              0, (AvahiLookupFlags)(0),
                                              _browseCBS, this );
        if( _browser )
            return servus::Servus::Result( _result );

        _result = avahi_client_errno( _client );
        WARN << "Failed to create browser for " << _name << ": "
             << avahi_strerror( _result ) << std::endl;
        return servus::Servus::Result( _result );
    }

    servus::Servus::Result browse( const int32_t timeout ) final
    {
        ScopedLock lock( _mutex );
        _result = servus::Servus::Result::PENDING;
        const chrono::high_resolution_clock::time_point& startTime =
            chrono::high_resolution_clock::now();

        do
        {
            if( avahi_simple_poll_iterate( _poll, timeout ) != 0 )
            {
                _result = servus::Servus::Result::POLL_ERROR;
                break;
            }
        }
        while( _elapsedMilliseconds( startTime ) < timeout );

        if( _result != servus::Servus::Result::POLL_ERROR )
            _result = servus::Servus::Result::SUCCESS;

        return servus::Servus::Result( _result );
    }

    void endBrowsing() final
    {
        ScopedLock lock( _mutex );
        if( _browser )
            avahi_service_browser_free( _browser );
        _browser = 0;
    }

    bool isBrowsing() const final { return _browser; }

    Strings discover( const ::servus::Servus::Interface addr,
                      const unsigned browseTime ) final
    {
        const servus::Servus::Result& result = beginBrowsing( addr );
        if( !result && result != servus::Servus::Result::PENDING )
            return getInstances();

        assert( _browser );
        browse( browseTime );
        if( result != servus::Servus::Result::PENDING )
            endBrowsing();
        return getInstances();
    }

    /* FIXME - expose protocol */
//    std::string &resolveServiceInstanceAddress(const servus::Servus::Interface interface_,
//                                               const std::string &inst) {
//
//        detail::ValueMap& values = _instanceMap[ inst ];
//        std::string host = values["servus_host"];
//
//        //avahi_host_name_resolver_new(_client, AVAHI_IF_UNSPEC, AVAHI_PROTO_INET, host.c_str(), AVAHI_PROTO_INET, AVAHI_LOOKUP_USE_WIDE_AREA | AVAHI_LOOKUP_USE_MULTICAST, NULL, NULL);
//        return "sdfsd";
//    }

private:
    AvahiSimplePoll* const _poll;
    AvahiClient* _client;
    AvahiServiceBrowser* _browser;
    AvahiEntryGroup* _group;
    int32_t _result;
    std::string _announce;
    unsigned short _port;
    servus::Servus::Interface _interface;
    bool _announcable;
    servus::Servus::Interface _scope;

    // Client state change
    static void _clientCBS( AvahiClient*, AvahiClientState state,
                            void* servus )
    {
        ((Servus*)servus)->_clientCB( state );
    }

    void _clientCB( AvahiClientState state )
    {
        switch (state)
        {
        case AVAHI_CLIENT_S_RUNNING:
            _announcable = true;
            if( !_announce.empty( ))
                _createServices();
            break;

        case AVAHI_CLIENT_FAILURE:
            _result = avahi_client_errno( _client );
            WARN << "Client failure: " << avahi_strerror( _result )
                 << std::endl;
            avahi_simple_poll_quit( _poll );
            break;

        case AVAHI_CLIENT_S_COLLISION:
            // Can't setup client
            _result = EEXIST;
            avahi_simple_poll_quit( _poll );
            break;

        case AVAHI_CLIENT_S_REGISTERING:
            // The server records are now being established. This might be
            // caused by a host name change. We need to wait for our own records
            // to register until the host name is properly established.
            throw std::runtime_error(
                "Unimplemented AVAHI_CLIENT_S_REGISTERING event" );
            // withdraw & _createServices ?
            break;

        case AVAHI_CLIENT_CONNECTING:
            /*nop*/;
        }
    }

    // Browsing
    static void _browseCBS( AvahiServiceBrowser*, AvahiIfIndex ifIndex,
                            AvahiProtocol protocol, AvahiBrowserEvent event,
                            const char* name, const char* type,
                            const char* domain, AvahiLookupResultFlags,
                            void* servus )
    {
        ((Servus*)servus)->_browseCB( ifIndex, protocol, event, name, type,
                                      domain );
    }

    void _browseCB( const AvahiIfIndex ifIndex, const AvahiProtocol protocol,
                    const AvahiBrowserEvent event, const char* name,
                    const char* type, const char* domain )
    {
        switch( event )
        {
        case AVAHI_BROWSER_FAILURE:
            _result = avahi_client_errno( _client );
            WARN << "Browser failure: " << avahi_strerror( _result )
                 << std::endl;
            avahi_simple_poll_quit( _poll );
            break;

        case AVAHI_BROWSER_NEW:
            // We ignore the returned resolver object. In the callback function
            // we free it. If the server is terminated before the callback
            // function is called the server will free the resolver for us.
            if( !avahi_service_resolver_new( _client, ifIndex, protocol, name,
                                             type, domain, AVAHI_PROTO_INET,
                                             (AvahiLookupFlags)(0),
                                             _resolveCBS, this ))
            {
                _result = avahi_client_errno( _client );
                WARN << "Error creating resolver: "
                     << avahi_strerror( _result ) << std::endl;
                avahi_simple_poll_quit( _poll );
                break;
            }

        case AVAHI_BROWSER_REMOVE:
            _instanceMap.erase( name );
            for( detail::Listeners::iterator i = _listeners.begin();
                 i != _listeners.end(); ++i )
            {
                (*i)->instanceRemoved( name );
            }
            break;

        case AVAHI_BROWSER_ALL_FOR_NOW:
        case AVAHI_BROWSER_CACHE_EXHAUSTED:
            _result = servus::Result::SUCCESS;
            break;
        }
    }

    static void _resolveCBS( AvahiServiceResolver* resolver,
                             AvahiIfIndex iFace, AvahiProtocol avahiProtocol,
                             AvahiResolverEvent event, const char* name,
                             const char*, const char*,
                             const char* host, const AvahiAddress* address,
                             uint16_t port, AvahiStringList *txt,
                             AvahiLookupResultFlags, void* servus )
    {
        ((Servus*)servus)->_resolveCB( resolver, iFace, avahiProtocol, event, name, host, address, port, txt);
    }

    void _resolveCB( AvahiServiceResolver* resolver,
                     AvahiIfIndex, AvahiProtocol,
                     const AvahiResolverEvent event, const char* name,
                     const char* host, const AvahiAddress* address,
                     uint16_t port, AvahiStringList *txt )
    {
        // If browsing through the local interface,
        // consider only the local instances
        if( _scope == servus::Servus::IF_LOCAL )
        {
            const std::string& hostStr( host );
            // host in "hostname.local" format
            const size_t pos = hostStr.find_last_of( "." );
            const std::string hostName = hostStr.substr( 0, pos );

            const std::string& localHost = getHostname();
            // omit the domain for the local hostname
            if( hostName != localHost.substr( 0, localHost.find_first_of( "." )))
                return;
        }

        switch( event )
        {
        case AVAHI_RESOLVER_FAILURE:
            _result = avahi_client_errno( _client );
            WARN << "Resolver error: " << avahi_strerror( _result )
                 << std::endl;
            break;

        case AVAHI_RESOLVER_FOUND:
            {
                detail::ValueMap& values = _instanceMap[ name ];
                values[ "servus_host" ] = host;
                values[ "servus_port" ] = ""+port;

                char str[INET_ADDRSTRLEN];

                inet_ntop(AF_INET, address->data.data, str, INET_ADDRSTRLEN);

                values[ "servus_address" ] = str;

                for( ; txt; txt = txt->next )
                {
                    const std::string entry(
                                reinterpret_cast< const char* >( txt->text ),
                                txt->size );
                    const size_t pos = entry.find_first_of( "=" );
                    const std::string key = entry.substr( 0, pos );
                    const std::string value = entry.substr( pos + 1 );
                    values[ key ] = value;
                }
                for( detail::Listeners::iterator i = _listeners.begin();
                     i != _listeners.end(); ++i )
                {
                    (*i)->instanceAdded( name );
                }
            } break;
        }

        avahi_service_resolver_free( resolver );
    }

    void _updateRecord() final
    {
        if( _announce.empty() || !_announcable )
            return;

        if( _group )
            avahi_entry_group_reset( _group );
        _createServices();
    }

    void _createServices()
    {
        if( !_group )
            _group = avahi_entry_group_new( _client, _groupCBS, this );
        else
            avahi_entry_group_reset( _group );

        if( !_group )
            return;

        AvahiStringList* data = 0;
        for( detail::ValueMapCIter i = _data.begin(); i != _data.end(); ++i )
            data = avahi_string_list_add_pair( data, i->first.c_str(),
                                               i->second.c_str( ));

        _result = avahi_entry_group_add_service_strlst(
            _group, _interface, AVAHI_PROTO_UNSPEC,
                (AvahiPublishFlags)(0), _announce.c_str(), _name.c_str(), 0, 0,
                _port, data );

        if( data )
            avahi_string_list_free( data );

        if( _result != servus::Result::SUCCESS )
        {
            avahi_simple_poll_quit( _poll );
            return;
        }

        _result = avahi_entry_group_commit( _group );
        if( _result != servus::Result::SUCCESS )
            avahi_simple_poll_quit( _poll );
    }

    static void _groupCBS( AvahiEntryGroup*, AvahiEntryGroupState state,
                           void* servus )
    {
        ((Servus*)servus)->_groupCB( state );
    }

    void _groupCB( const AvahiEntryGroupState state )
    {
        switch( state )
        {
        case AVAHI_ENTRY_GROUP_ESTABLISHED:
            break;

        case AVAHI_ENTRY_GROUP_COLLISION:
        case AVAHI_ENTRY_GROUP_FAILURE:
            _result = EEXIST;
            avahi_simple_poll_quit( _poll );
            break;

        case AVAHI_ENTRY_GROUP_UNCOMMITED:
        case AVAHI_ENTRY_GROUP_REGISTERING:
            /*nop*/ ;
        }
    }
};

}
}
