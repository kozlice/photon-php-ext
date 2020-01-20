/**
 * Start web server with UDP transport:
 * php -d extension=photon.so -d photon.agent_transport=udp -S localhost:8000
 *
 * Run CLI script with UDP transport:
 * php -d extension=photon.so -d photon.agent_transport=udp test.php
 */

const PORT = 8989;
const HOST = '127.0.0.1';

const dgram = require('dgram');
const server = dgram.createSocket('udp4');

server.on('listening', function() {
    const address = server.address();
    console.log('UDP Server listening on ' + address.address + ':' + address.port);
});

server.on('message', function(message, remote) {
    console.log('Incoming from ' + remote.address + ':' + remote.port +': ' + message);
});

server.bind(PORT, HOST);
