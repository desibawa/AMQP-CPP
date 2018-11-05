/**
 *  TcpConnected.h
 * 
 *  The actual tcp connection - this is the "_impl" of a tcp-connection after
 *  the hostname was resolved into an IP address
 * 
 *  @author Emiel Bruijntjes <emiel.bruijntjes@copernica.com>
 *  @copyright 2015 - 2018 Copernica BV
 */

/**
 *  Include guard
 */
#pragma once

/**
 *  Dependencies
 */
#include "tcpoutbuffer.h"
#include "tcpinbuffer.h"
#include "tcpshutdown.h"
#include "poll.h"

/**
 *  Set up namespace
 */
namespace AMQP {

/**
 *  Class definition
 */
class TcpConnected : public TcpExtState
{
private:
    /**
     *  The outgoing buffer
     *  @var TcpOutBuffer
     */
    TcpOutBuffer _out;
    
    /**
     *  An incoming buffer
     *  @var TcpInBuffer
     */
    TcpInBuffer _in;
    
    /**
     *  Cached reallocation instruction
     *  @var size_t
     */
    size_t _reallocate = 0;
    
    /**
     *  Have we already made the last report to the user (about an error or closed connection?)
     *  @var bool
     */
    bool _finalized = false;

    
    /**
     *  Start an elegant shutdown
     * 
     *  @todo remove this method
     */
    void shutdown2()
    {
        // we will shutdown the socket in a very elegant way, we notify the peer 
        // that we will not be sending out more write operations
        ::shutdown(_socket, SHUT_WR);
        
        // we still monitor the socket for readability to see if our close call was
        // confirmed by the peer
        _parent->onIdle(this, _socket, readable);
    }
    
    /**
     *  Helper method to report an error
     *  @param  monitor     Monitor to check validity of "this"
     *  @return bool        Was an error reported?
     */
    bool reportError(const Monitor &monitor)
    {
        // some errors are ok and do not (necessarily) mean that we're disconnected
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return false;

        // tell the connection that it failed
        // @todo we should report an error, but that could be wrong, because it calls back to us

        // we're no longer interested in the socket (this also calls onClosed())
        cleanup();
        
        // done
        return true;
    }
    
    /**
     *  Construct the next state
     *  @param  monitor     Object that monitors whether connection still exists
     *  @return TcpState*
     */
    TcpState *nextState(const Monitor &monitor)
    {
        // if the object is still in a valid state, we can move to the close-state, 
        // otherwise there is no point in moving to a next state
        return monitor.valid() ? new TcpClosed(this) : nullptr;
    }
    
public:
    /**
     *  Constructor
     *  @param  state       The previous state
     *  @param  buffer      The buffer that was already built
     */
    TcpConnected(TcpExtState *state, TcpOutBuffer &&buffer) : 
        TcpExtState(state),
        _out(std::move(buffer)),
        _in(4096)
    {
        // if there is already an output buffer, we have to send out that first
        if (_out) _out.sendto(_socket);
        
        // tell the handler to monitor the socket, if there is an out
        _parent->onIdle(this, _socket, _out ? readable | writable : readable);
    }
    
    /**
     *  Destructor
     */
    virtual ~TcpConnected() noexcept = default;

    /**
     *  The filedescriptor of this connection
     *  @return int
     */
    virtual int fileno() const override { return _socket; }

    /**
     *  Number of bytes in the outgoing buffer
     *  @return std::size_t
     */
    virtual std::size_t queued() const override { return _out.size(); }

    /**
     *  Process the filedescriptor in the object
     *  @param  monitor     Monitor to check if the object is still alive
     *  @param  fd          Filedescriptor that is active
     *  @param  flags       AMQP::readable and/or AMQP::writable
     *  @return             New state object
     */
    virtual TcpState *process(const Monitor &monitor, int fd, int flags) override
    {
        // must be the socket
        if (fd != _socket) return this;

        // can we write more data to the socket?
        if (flags & writable)
        {
            // send out the buffered data
            auto result = _out.sendto(_socket);
            
            // are we in an error state?
            if (result < 0 && reportError(monitor)) return nextState(monitor);
            
            // if buffer is empty by now, we no longer have to check for 
            // writability, but only for readability
            if (!_out) _parent->onIdle(this, _socket, readable);
        }
        
        // should we check for readability too?
        if (flags & readable)
        {
            // read data from buffer
            ssize_t result = _in.receivefrom(_socket, _parent->expected());
            
            // are we in an error state?
            if (result < 0 && reportError(monitor)) return nextState(monitor);
            
            // @todo should we also check for result == 0

            // we need a local copy of the buffer - because it is possible that "this"
            // object gets destructed halfway through the call to the parse() method
            TcpInBuffer buffer(std::move(_in));
            
            // parse the buffer
            auto processed = _parent->onReceived(this, buffer);

            // "this" could be removed by now, check this
            if (!monitor.valid()) return nullptr;
            
            // shrink buffer
            buffer.shrink(processed);
            
            // restore the buffer as member
            _in = std::move(buffer);
            
            // do we have to reallocate?
            if (_reallocate) _in.reallocate(_reallocate); 
            
            // we can remove the reallocate instruction
            _reallocate = 0;
        }
        
        // keep same object
        return this;
    }

    /**
     *  Send data over the connection
     *  @param  buffer      buffer to send
     *  @param  size        size of the buffer
     */
    virtual void send(const char *buffer, size_t size) override
    {
        // is there already a buffer of data that can not be sent?
        if (_out) return _out.add(buffer, size);

        // there is no buffer, send the data right away
        auto result = ::send(_socket, buffer, size, AMQP_CPP_MSG_NOSIGNAL);

        // number of bytes sent
        size_t bytes = result < 0 ? 0 : result;

        // ok if all data was sent
        if (bytes >= size) return;
    
        // add the data to the buffer
        _out.add(buffer + bytes, size - bytes);
        
        // start monitoring the socket to find out when it is writable
        _parent->onIdle(this, _socket, readable | writable);
    }
    
    /**
     *  Flush the connection, sent all buffered data to the socket
     *  @param  monitor     Object to check if connection still lives
     *  @return TcpState    new tcp state
     */
    virtual TcpState *flush(const Monitor &monitor) override
    {
        // create an object to wait for the filedescriptor to becomes active
        Poll poll(_socket);

        // keep running until the out buffer is not empty
        while (_out)
        {
            // poll the socket, is it already writable?
            if (!poll.writable(true)) return this;
            
            // socket is writable, send as much data as possible
            auto *newstate = process(monitor, _socket, writable);
            
            // are we done
            if (newstate != this) return newstate;
        }
        
        // all has been sent
        return this;
    }
    
    /**
     *  When the AMQP transport layer is closed
     *  @param  monitor     Object that can be used if connection is still alive
     *  @return TcpState    New implementation object
     */
    virtual TcpState *onAmqpClosed(const Monitor &monitor) override
    {
        // move to the tcp shutdown state
        return new TcpShutdown(this);
    }

    /**
     *  When an error occurs in the AMQP protocol
     *  @param  monitor     Monitor that can be used to check if the connection is still alive
     *  @param  message     The error message
     *  @return TcpState    New implementation object
     */
    virtual TcpState *onAmqpError(const Monitor &monitor, const char *message) override
    {
        // tell the user about it
        // @todo do this somewhere else
        //_handler->onError(_connection, message);
        
        // stop if the object was destructed
        if (!monitor.valid()) return nullptr;
        
        // move to the tcp shutdown state
        return new TcpShutdown(this);
    }

    /**
     *  Install max-frame size
     *  @param  heartbeat   suggested heartbeat
     */
    virtual void maxframe(size_t maxframe) override
    {
        // remember that we have to reallocate (_in member can not be accessed because it is moved away)
        _reallocate = maxframe;
    }
};
    
/**
 *  End of namespace
 */
}

