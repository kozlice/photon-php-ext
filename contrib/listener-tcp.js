/**
 * Start web server with TCP transport:
 * php -d extension=photon.so -d photon.agent_transport=tcp -d photon.agent_port=8990 -S localhost:8000
 *
 * Run CLI script with TCP transport:
 * php -d extension=photon.so -d photon.agent_transport=tcp -d photon.agent_port=8990 test.php
 */

const PORT = 8990;
const HOST = '127.0.0.1';

const net = require('net');

const server = net.createServer(function(stream) {
    console.log('Connection acknowledged');

    stream.on('data', function(msg) {
        msg = msg.toString();
        console.log('Incoming: ' + msg);
    });

    stream.on('end', function() {
        console.log('Client disconnected');
    });
});

server.on('listening', function() {
    const address = server.address();
    console.log('TCP server listening on ' + address.address + ':' + address.port);
});

server.listen(PORT, HOST);

server.on('connection', function(socket) {
    console.log('New client connected');
});

process.on('SIGINT', function() {
    // Graceful shutdown, removes .sock file
    server.close();
});
