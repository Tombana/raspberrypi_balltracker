#!/usr/bin/env python3

from websocket_server import WebsocketServer

import os
import threading
import time

# Called for every client connecting (after handshake)
def new_client(client, server):
	print("New client connected and was given id %d" % client['id'])
	server.send_message_to_all("Hey all, a new client has joined us")


# Called for every client disconnecting
def client_left(client, server):
	print("Client(%d) disconnected" % client['id'])


# Called when a client sends a message
def message_received(client, server, message):
	if len(message) > 200:
		message = message[:200]+'..'
	print("Client(%d) said: %s" % (client['id'], message))


PORT=8420
server = WebsocketServer(PORT)
server.set_fn_new_client(new_client)
server.set_fn_client_left(client_left)
server.set_fn_message_received(message_received)


class MyThread(threading.Thread):
    def run(self):
        print("Thread started.")
        fifo_file = "/tmp/foos-debug.in"
        try:
            os.mkfifo(fifo_file)
        except:
            pass
        while True:
            f = open(fifo_file, "r")
            if not f:
                print("Error opening fifo file %s" % fifo_file)
                time.sleep(5)
                continue
            print("Fifo file opened.")
            while True:
                line = f.readline()
                if not line:
                    break
                line = line.strip()
                print("Message from fifo file: %s" % line)
                server.send_message_to_all(line);


mythread = MyThread()
mythread.start()

server.run_forever()
