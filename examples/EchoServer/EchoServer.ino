
/***************************************************
  Adafruit CC3000 Breakout/Shield TCP Echo Server
    
  This is a simple implementation of the echo 
  protocol, RFC 862 http://tools.ietf.org/html/rfc862 , 
  for the Arduino platform and Adafruit CC3000 breakout
  or shield.  This sketch will create a TCP server that 
  listens by default on port 7 and echos back any data
  received.  Up to 3 clients can be connected concurrently
  to the server.  This sketch is meant as an example of how 
  to write a simple server with the Arduino and CC3000.

  See the CC3000 tutorial on Adafruit's learning system
  for more information on setting up and using the
  CC3000:
    http://learn.adafruit.com/adafruit-cc3000-wifi  
    
  Requirements:
  
  This sketch requires the Adafruit CC3000 library.  You can
  download the library from:
    https://github.com/adafruit/Adafruit_CC3000_Library
  
  For information on installing libraries in the Arduino IDE
  see this page:
    http://arduino.cc/en/Guide/Libraries
  
  Usage:
    
  Update the SSID and, if necessary, the CC3000 hardware pin 
  information below, then run the sketch and check the 
  output of the serial port.  After connecting to the 
  wireless network successfully the sketch will output 
  the IP address of the server and start listening for 
  connections.  Once listening for connections, connect
  to the server from your computer  using a telnet client
  on port 7.  
           
  For example on Linux or Mac OSX, if your CC3000 has an
  IP address 192.168.1.100 you would execute in a command
  window:
  
    telnet 192.168.1.100 7
           
  After connecting, notice that as you type input and 
  press enter to send it the CC3000 will echo back exactly
  what you typed.  Press ctrl-] and type quit at the prompt 
  to close the telnet session.
           
  On Windows you'll need to download a telnet client.  PuTTY 
  is a good, free GUI client: 
    http://www.chiark.greenend.org.uk/~sgtatham/putty/
  
  License:
 
  This example is copyright (c) 2013 Tony DiCola (tony@tonydicola.com)
  and is released under an open source MIT license.  See details at:
    http://opensource.org/licenses/MIT
  
  This code was adapted from Adafruit CC3000 library example 
  code which has the following license:
  
  Designed specifically to work with the Adafruit WiFi products:
  ----> https://www.adafruit.com/products/1469

  Adafruit invests time and resources providing this open source code, 
  please support Adafruit and open-source hardware by purchasing 
  products from Adafruit!

  Written by Limor Fried & Kevin Townsend for Adafruit Industries.  
  BSD license, all text above must be included in any redistribution      
 ****************************************************/
#include <Adafruit_CC3000.h>
#include <SPI.h>
#include "utility/debug.h"
#include "utility/socket.h"

// These are the interrupt and control pins
#define ADAFRUIT_CC3000_IRQ   3  // MUST be an interrupt pin!
// These can be any two pins
#define ADAFRUIT_CC3000_VBAT  5
#define ADAFRUIT_CC3000_CS    10
// Use hardware SPI for the remaining pins
// On an UNO, SCK = 13, MISO = 12, and MOSI = 11
Adafruit_CC3000 cc3000 = Adafruit_CC3000(ADAFRUIT_CC3000_CS, ADAFRUIT_CC3000_IRQ, ADAFRUIT_CC3000_VBAT,
                                         SPI_CLOCK_DIV2); // you can change this clock speed

#define WLAN_SSID       "myNetwork"           // cannot be longer than 32 characters!
#define WLAN_PASS       "myPassword"
// Security can be WLAN_SEC_UNSEC, WLAN_SEC_WEP, WLAN_SEC_WPA or WLAN_SEC_WPA2
#define WLAN_SECURITY   WLAN_SEC_WPA2

#define LISTEN_PORT           7    // What TCP port to listen on for connections.  The echo protocol uses port 7.
#define MAX_CLIENTS           3    // The CC3000 docs advise that it has 4 sockets available to client code.
                                   // One socket is consumed listening for new connections, and the remaining sockets
                                   // are available for client connections.
                                   // In practice it appears you can go higher than 4 sockets (up to about 7), but
                                   // be careful as it might cause the CC3000 to behave unexpectedly.

// Define a simple forward linked list structure to keep track of 
// connected clients.  This client list is kept as a linked list 
// because the CC3000 client instances have a non-trivial amount of 
// internal state (buffers, etc.) which we don't want to spend memory 
// consuming unless a client is connected.  Because the size of the list
// is small and random access to entries is rare (happens only on
// disconnect) a forward list (i.e. no back/previous pointer) is
// sufficient.
struct ClientList {
  int socket;
  Adafruit_CC3000_Client client;
  ClientList* next;
};

// Global variable to hold the ID of the listening socket.
int listenSocket;

// Global variable to hold the start of the connected client list.
ClientList* clients;

// Global variable to keep track of how many clients are connected.
int clientCount;

// Function to add a new client to the connected client list.
void addNewClient(int socket) {
  // Allocate memory for a new list entry.
  // In general be very careful with heap/dynamic memory use in an Arduino sketch
  // because the function stack and heap share the same limited amount of memory and
  // can easily overflow.  In this case the number of connected clients is low
  // so the risk of using up all the memory is low.
  ClientList* client = (ClientList*) malloc(sizeof(ClientList));
  if (client == NULL) {
    Serial.println(F("Error! Couldn't allocate space to store a new client."));
    return;
  }
  // Setup the new client as the front of the connected client list.
  client->next = clients;
  client->socket = socket;
  client->client = Adafruit_CC3000_Client(socket);
  clients = client;
  // Increment the count of connected clients.
  clientCount++;
}

// Remove a client from the connected client list.
void removeClient(struct ClientList* client) {
  if (client == NULL) {
    // Handle null client to delete.  This should never happen
    // but is good practice as a precaution.
    return;
  }
  if (clients == client) {
    // Handle the client to delete being at the front of the list.
    clients = client->next;
  }
  else {
    // Handle the client to delete being somewhere inside the list.
    // Iterate through the list until we find the entry before the
    // client to delete.
    ClientList* i = clients;
    while (i != NULL && i->next != client) {
      i = i->next;
    }
    if (i == NULL) {
      // Couldn't find the client to delete, do nothing.
      return;
    }
    // Remove the client from the list.
    i->next = client->next;
  }
  // Free the memory associated with the client and set the
  // pointer to null as a precaution against dangling references.
  free(client);
  client = NULL;
  // Decrement the count of connected clients.
  clientCount--;
}

// Set up the echo server and start listening for connections.  Should be called once
// in the setup function of the Arduino sketch.
void echoSetup() {
  // Most of the calls below are to CC3000 firmware API's. You can find the documentaiton for these calls at:
  //   http://software-dl.ti.com/ecs/simplelink/cc3000/public/doxygen_API/v1.11.1/html/index.html
  
  // Set the CC3000 inactivity timeout to 0 (never timeout).  This will ensure the CC3000
  // does not close the listening socket when it's idle for more than 60 seconds (the
  // default timeout).  See more information from:
  //   http://e2e.ti.com/support/low_power_rf/f/851/t/292664.aspx
  unsigned long aucDHCP       = 14400;
  unsigned long aucARP        = 3600;
  unsigned long aucKeepalive  = 30;
  unsigned long aucInactivity = 0;
  if (netapp_timeout_values(&aucDHCP, &aucARP, &aucKeepalive, &aucInactivity) != 0) {
    Serial.println(F("Error setting inactivity timeout!"));
    while(1);
  }
  // Create a TCP socket
  listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listenSocket < 0) {
    Serial.println(F("Couldn't create listening socket!"));
    while(1);
  }
  // Set the socket's accept call as non-blocking.
  // This is required to support multiple clients accessing the server at once.  If the listening
  // port is not set as non-blocking your code can't do anything while it waits for a client to connect.
  if (setsockopt(listenSocket, SOL_SOCKET, SOCKOPT_ACCEPT_NONBLOCK, SOCK_ON, sizeof(SOCK_ON)) < 0) {
    Serial.println(F("Couldn't set socket as non-blocking!"));
    while(1);
  }
  // Bind the socket to a TCP address.
  sockaddr_in address;
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(0);     // Listen on any network interface, equivalent to INADDR_ANY in sockets programming.
  address.sin_port = htons(LISTEN_PORT);  // Listen on the specified port.
  if (bind(listenSocket, (sockaddr*) &address, sizeof(address)) < 0) {
    Serial.println(F("Error binding listen socket to address!"));
    while(1);
  }
  // Start listening for connectings.
  // The backlog parameter is 0 as it is not supported on TI's CC3000 firmware.
  if (listen(listenSocket, 0) < 0) {
    Serial.println(F("Error opening socket for listening!"));
    while(1);
  }
  // Initialize client list as empty.
  clients = NULL;
  clientCount = 0;
  Serial.println(F("Listening for connections..."));
}

// Update the state of clients, and accept new client connections.  Should be called
// by the Arduino sketch's loop function.
void echoLoop() {  
  // Iterate through all the connected clients.
  ClientList* i = clients;
  while (i != NULL) {
    // Save the next client so the current one can be removed when it disconnects
    // without breaking iteration through the clients.
    ClientList* next = i->next;
    // If there's data available, read it a character at a time from the
    // CC3000 library's internal buffer.
    while (i->client.available() > 0) {
      uint8_t ch = i->client.read();
      // Echo the read byte back out to the client immediately.
      if (i->client.write(ch) == 0) {
        Serial.println(F("Error writing character to client!"));
      }
    }
    // Check if the client is disconnected and remove it from the active client list.
    if (!i->client.connected()) {
      Serial.print(F("Client on socket "));
      Serial.print(i->socket);
      Serial.println(F(" disconnected."));
      removeClient(i);
      // Note that i is now NULL!  Don't try to dereference it or you will
      // have a bad day (your Arduino will reset).
    }
    // Continue iterating through clients.
    i = next;
  }
  // Handle new client connections if we aren't at the limit of connected clients.
  if (clientCount < MAX_CLIENTS) {
    // Accept a new socket connection.  Because we set the listening socket to be non-blocking 
    // this will quickly return a result with either a new socket, an indication that nothing
    // is trying to connect, or an error.
    // The NULL parameters allow you to read the address of the connected client but are
    // unused in this sketch.  See TI's documentation (linked in the setup function) for
    // more details.
    int newSocket = accept(listenSocket, NULL, NULL);
    // Check if a client is connected to a new socket.
    if (newSocket > -1) {
      Serial.print(F("New client connected on socket "));
      Serial.println(newSocket);
      // Add the client to the list of connected clients.
      addNewClient(newSocket);
    }
  }
}

void setup(void)
{
  Serial.begin(115200);
  Serial.println(F("Hello, CC3000!\n")); 

  Serial.print("Free RAM: "); Serial.println(getFreeRam(), DEC);
  
  /* Initialise the module */
  Serial.println(F("\nInitializing..."));
  if (!cc3000.begin())
  {
    Serial.println(F("Couldn't begin()! Check your wiring?"));
    while(1);
  }
  
  if (!cc3000.connectToAP(WLAN_SSID, WLAN_PASS, WLAN_SECURITY)) {
    Serial.println(F("Failed!"));
    while(1);
  }
   
  Serial.println(F("Connected!"));
  
  Serial.println(F("Request DHCP"));
  while (!cc3000.checkDHCP())
  {
    delay(100); // ToDo: Insert a DHCP timeout!
  }  

  /* Display the IP address DNS, Gateway, etc. */  
  while (! displayConnectionDetails()) {
    delay(1000);
  }
  
  // Initialize the echo server
  echoSetup();
}

void loop(void)
{
  // Update the echo server.
  echoLoop();
}

/**************************************************************************/
/*!
    @brief  Tries to read the IP address and other connection details
*/
/**************************************************************************/
bool displayConnectionDetails(void)
{
  uint32_t ipAddress, netmask, gateway, dhcpserv, dnsserv;
  
  if(!cc3000.getIPAddress(&ipAddress, &netmask, &gateway, &dhcpserv, &dnsserv))
  {
    Serial.println(F("Unable to retrieve the IP Address!\r\n"));
    return false;
  }
  else
  {
    Serial.print(F("\nIP Addr: ")); cc3000.printIPdotsRev(ipAddress);
    Serial.print(F("\nNetmask: ")); cc3000.printIPdotsRev(netmask);
    Serial.print(F("\nGateway: ")); cc3000.printIPdotsRev(gateway);
    Serial.print(F("\nDHCPsrv: ")); cc3000.printIPdotsRev(dhcpserv);
    Serial.print(F("\nDNSserv: ")); cc3000.printIPdotsRev(dnsserv);
    Serial.println();
    return true;
  }
}
