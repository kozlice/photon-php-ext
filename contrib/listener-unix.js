/**
 * Start web server with UNIX socket transport:
 * php -d extension=photon.so -d photon.agent_transport=unix -d photon.agent_socket_path=/tmp/photon-agent.sock -S localhost:8000
 *
 * Run CLI script with UNIX socket transport:
 * php -d extension=photon.so -d photon.agent_transport=unix -d photon.agent_socket_path=/tmp/photon-agent.sock test.php
 */

const SOCKET_FILE_PATH = '/tmp/photon-agent.sock';

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
    console.log('Unix socket server listening ' + SOCKET_FILE_PATH);
});

server.listen(SOCKET_FILE_PATH);

server.on('connection', function(socket) {
    console.log('New client connected');
});

process.on('SIGINT', function() {
    // Graceful shutdown, removes .sock file
    server.close();
});
