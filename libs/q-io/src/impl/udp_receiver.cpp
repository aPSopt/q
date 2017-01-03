/*
 * Copyright 2016 Gustaf Räntilä
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "udp_receiver.hpp"
#include "dispatcher.hpp"

namespace q { namespace io {

namespace {

static void closer( ::uv_handle_t* handle )
{
	auto socket = reinterpret_cast< ::uv_udp_t* >( handle );
	auto ref = reinterpret_cast< udp_receiver::pimpl::data_ref_type* >(
		socket->data );
	socket->data = nullptr;

	if ( ref )
		delete ref;
};

} // anonymous namespace

std::shared_ptr< udp_receiver::pimpl >
udp_receiver::pimpl::construct(
	queue_ptr user_queue, std::uint16_t port, udp_receive_options options
)
{
	auto pimpl = q::make_shared_using_constructor< udp_receiver::pimpl >( );
	pimpl->self_ = pimpl;

	std::size_t bl = std::numeric_limits< std::size_t >::max( );
	if ( options.has< q::backlog >( ) )
	{
		auto backlog = options.get< q::backlog >( );
		bl = std::max< std::size_t >( backlog.get( ), 1 );

		pimpl->is_infinite_ = backlog.is_infinity( );
	}
	else
		pimpl->is_infinite_ = true;

	pimpl->port_ = port;

	pimpl->construction_options_ = q::make_unique< udp_receive_options >(
		std::move( options ) );

	q::channel< udp_packet > ch( user_queue, bl, bl - 1 );

	pimpl->readable_in_ = std::make_shared< q::readable< udp_packet > >(
		ch.get_readable( ) );
	pimpl->writable_in_ = std::make_shared< q::writable< udp_packet > >(
		ch.get_writable( ) );

	return pimpl;
}

void
udp_receiver::pimpl::attach_dispatcher( const dispatcher_ptr& dispatcher )
noexcept
{
	auto u_ref = q::make_unique< data_ref_type >( shared_from_this( ) );

	udp_.data = reinterpret_cast< void* >( u_ref.release( ) );

	::uv_udp_init( &dispatcher->pimpl_->uv_loop, &udp_ );

	auto bind_to = construction_options_->template get< ip_address >(
		ip_address( "0.0.0.0" ) );
	auto bind_flags = construction_options_->template get< udp_bind >( );

	auto sockaddr = bind_to.get_sockaddr( port_ );

	unsigned int flags = 0
		| ( Q_ENUM_HAS( bind_flags, udp_bind::ip_v6_only )
			? UV_UDP_IPV6ONLY : 0 )
		| ( Q_ENUM_HAS( bind_flags, udp_bind::reuseaddr )
			? UV_UDP_REUSEADDR : 0 );

	::uv_udp_bind( &udp_, sockaddr.get( ), flags );

	construction_options_.reset( );

	start_read( );
}

// TODO: make this function "i_close" and ensure outside calls (in public class
//       destructors e.g.) schedule this on the internal thread.
void udp_receiver::pimpl::close( expect< void > status )
{
	if ( closed_.exchange( true ) )
		return;

	auto writable_in = std::atomic_load( &writable_in_ );

	if ( !!writable_in )
	{
		writable_in->unset_resume_notification( );
		if ( status.has_exception( ) )
			writable_in->close( status.exception( ) );
		else
			writable_in->close( );
	}

	stop_read( );

	writable_in_.reset( );

	auto handle = reinterpret_cast< ::uv_handle_t* >( &udp_ );
	::uv_close( handle, closer );
}

void udp_receiver::pimpl::start_read( )
{
	auto alloc_cb = [ ](
		::uv_handle_t* handle,
		::size_t suggested_size,
		::uv_buf_t* buf
	)
	{
		buf->base = new char[ suggested_size ];
		buf->len = suggested_size;
	};

	auto read_cb = [ ](
		::uv_udp_t* udp,
		::ssize_t nread,
		const uv_buf_t* buf,
		const struct sockaddr* addr,
		unsigned flags
	)
	{
		auto pimpl = *reinterpret_cast< data_ref_type* >( udp->data );

		if ( addr )
		{
			// Did receive something, although maybe an empty
			// packet, or an erroneous transmission.

			auto data = reinterpret_cast< const std::uint8_t* >(
				buf->base );

			udp_packet packet{
				nread < 0
					? q::refuse< q::byte_block >(
						std::make_exception_ptr(
							udp_packet_error( ) ) )
					: q::fulfill( nread == 0
						? q::byte_block( )
						: q::byte_block( nread, data )
					),
				ip_address::from( addr ),
				0
			};

			if ( !pimpl->writable_in_->write( std::move( packet ) ) )
				pimpl->stop_read( false );
			else if ( !pimpl->is_infinite_ )
			{
				if ( !pimpl->writable_in_->should_write( ) )
					pimpl->stop_read( true );
			}

			return;
		}

		// If a buffer has been allocated but we can't read (because
		// closed connection or error), we need to clean it up.
		if ( buf->base )
			delete[ ] buf->base;
	};

	::uv_udp_recv_start( &udp_, alloc_cb, read_cb );
}

void udp_receiver::pimpl::stop_read( bool reschedule )
{
	::uv_udp_recv_stop( &udp_ );

	if ( !reschedule )
		return;

	auto weak_pimpl = self_;

	writable_in_->set_resume_notification(
		[ weak_pimpl ]( )
		{
			auto pimpl = weak_pimpl.lock( );
			if ( pimpl )
				pimpl->start_read( );
		},
		true
	);
}

void udp_receiver::pimpl::detach( )
{
	auto self = shared_from_this( );

	auto scope = [ self ]( ) mutable
	{
		auto w = std::make_shared< writable< udp_packet > >( );
		auto r = std::make_shared< readable< udp_packet > >( );

		std::atomic_store( &self->writable_in_, w );
		std::atomic_store( &self->readable_in_, r );

		self->close( );
	};

	if ( detached_.exchange( true ) )
		// Already detached
		return;

	readable_in_->add_scope_until_closed(
		q::make_scoped_function( std::move( scope ) ) );
}

} } // namespace io, namespace q