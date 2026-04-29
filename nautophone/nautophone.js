#!/usr/bin/env node

//Status checker for MistServer Foghorn
//Named Nautophone to add to confusion!

//The Status checker will listen on an HTTP port
//Once reached it will read out the parameters and decide on which MistServer Foghorn should be checked. 
//It will then send out an UDP signal pretending to be an endpoint to connect to.
//This will receive a reply back from the Foghorn server, which will be used to determine whether it is up/down
//Should no reply come within the timeout time then we consider the server down.


//name to claim at Foghorn, if this is silly Jaron did not code-check this or agreed with the name
let mapname = 'iamtheonewhoknocks'

//Http port to listen on requests for
let hport = '7078'

//  For example:
//
//  http://localhost:7078/?host=foghorn-is-here.com&port=1234&passphrase=letmein
//  Supported url params are:
//    - host: the url at which we can find the foghorn instance you want information from
//    - port: the port foghorn is running at - defaults to 7077
//    - pass: the passphrase to use - default is none
//    - cmd: what command to send:
//      - 0:    Request being indexed and being told public address. (Can be used to check if the foghorn instance is up)
//      - 4:    Ask for a pong reply to the given port (Note: sending a pong is currently not implemented by foghorn)
//      - 255:  Get list of endpoints and their states (Note: if there are no endpoints known, foghorn will not reply)
//    - getlist: alias for cmd=255, no value required
//    - machine: active JSON output mode
//
//    When using getlist or cmd=255, the following parameters can be used:
//    - start: Start listing entries from this index - default is 0
//    - end:   Stop listing entries after this index - default is 1 (Note: entries will be listed until the UDP packet it is being sent in is full)
//
//    If the foghorn instance could not be reached (before the timeout), the page will return a 404 status code. Otherwise it will return 200 as normal. 
//    When retrieving the list of endpoints, this page will listen for foghorn's replies until it has received all entries it has asked for. If it has received some (but not all) at the timeout, it will list the messages it did receive as well as return a 404 code.
//
//  FOGHORN PROTOCOL documentation: 
//    https://hedgedoc.ddvtech.com/0NKBErdJTaOIxqynki-hJw

const encoder = new TextEncoder();

function encodeIntoAtPosition(string, u8array, position) {
    return encoder.encodeInto(
        string,
        position ? u8array.subarray(position | 0) : u8array,
    );
}
function randomBetween(min, max) { // min and max included 
  return Math.floor(Math.random() * (max - min + 1) + min);
}
function intToByteArray(num,length) {
  let a = new Uint8Array(length);

  for (let i = 1; i <= length; i++) {
    a[length-i] = num % 256;
    num = Math.floor(num / 256);
  }

  return a;
}
function arrayToInt(arr) {
  let num = 0;
  for (i = 0; i < arr.length; i++) {
    num += arr[arr.length-1-i] * Math.pow(256,i);
  }
  return num;
}
function HRstate(statebyte) {
  switch (statebyte) {
    case 0:   return "Open";
    case 1:   return "Consistent";
    case 2:   return "Predictable";
    case 254: return "Impenetrable";
    case 255: return "Unknown";
  }
  return "Unknown state";
}
function HRip(ip) {
  if (ip.length == 4) {
    //IPv4
    return [ip[0],ip[1],ip[2],ip[3]].join(".");
  }
  //IPv6

  let str = Buffer.from(ip).toString("hex");

  //split into groups of 4 characters each
  let ip6 = [];
  for (let i = 0; i < str.length; i = i + 4) {
    ip6.push(str.slice(i,i+4));
  }

  //remove leading 0s
  for (let i in ip6) {
    let section = ip6[i];
    let out = section;
    for (let n = 0; n < 3; n++) {
      if (section[n] == "0") {
        out = out.substring(1);
      }
      else { break; }
    }
    ip6[i] = out;
  }

  let firstpart = ip6.slice(0,6).join(":");
  if ((firstpart == "64:ff9b:0:0:0:0") || (firstpart == "0:0:0:0:0:ffff")) {
    //the last two groups actually display an ipv4. Convert and join with dots
    let ipv4 = HRip(ip.slice(-4));
    ip6.splice(-2);
    ip6.push(ipv4);
  }

  //collapse the largest group of 0s
  let longest = 0;
  let longest_index = -1; 
  for (let i = 0; i < ip6.length; i++) {
    if (ip6[i] != "0") { continue; }

    //count the length of this group of 0s
    let l = 1;
    while (ip6[i+l] == "0") {
      l++;
    }
    if (l > longest) {
      longest = l;
      longest_index = i;
    }
    i += l-1;
  }
  if (longest_index > -1) {
    ip6.splice(longest_index,longest,longest_index > 0 ? "" : ":");
    if (ip6.length == 1) { ip6.push(""); }
  }

  str = ip6.join(":");

  return str;

}


// End of Info/Settings/Variables code starts here

//Enable HTTP
var http = require('http');
var dgram = require('dgram');

//Crypto definitely for SHA256 and not a bitcoinminer
const {
    createHash
} = require('crypto');
//create a server object:
http.createServer(async function(req, res) {
    //Override defaults if endpoint is poked with server:port/passphrase included.
    if (req.url.includes("favicon")) {
        return;
    }

    let host, port, passphrase = "";
    let command = 0;
    let mode = "human";
    
    let url = new URL(`http://${process.env.HOST ?? 'localhost'}${req.url}`); 
    if (url.searchParams) {
      if (url.searchParams.has("host")) { host = url.searchParams.get("host"); }
      if (url.searchParams.has("port")) { port = url.searchParams.get("port"); }
      if (url.searchParams.has("pass")) { passphrase = url.searchParams.get("pass"); }
      if (url.searchParams.has("cmd"))  { command = Number(url.searchParams.get("cmd")); }
      if (url.searchParams.has("getlist"))  { command = 255; }
      if (url.searchParams.has("machine"))  { mode = "machine"; }
    }
    
    if (!port) {
      port = 7077;
    }
    if (!host) {
      res.writeHead(404, {
        'Access-Control-Allow-Origin': "*",
        "Content-Type": "text/html;charset=UTF8"
      });
      res.write(`
        <h1>Hello! You've reached Nautophone</h1>
        <p>You've not provided a host so you probably don't know how to use this page. Let me tell you how!</p>
        <p>For example, use:</p>
        <p><a href="/?host=foghorn-is-here.com&port=1234&pass=letmein">http://example.com:7078/?host=foghorn-is-here.com&port=1234&pass=letmein</a></p>

        <p>Supported url params are:</p>
        <ul>
          <li><code>host</code>: the url at which we can find the foghorn instance you want information from</li>
          <li><code>port</code>: the port foghorn is running at - defaults to <code>7077</code></li>
          <li><code>pass</code>: the passphrase to use - default is none</li>
          <li>
            <code>cmd</code>: what command to send: - default is <code>0</code>
            <ul>
              <li><code>0</code>:    Request being indexed and being told public address. (Can be used to check if the foghorn instance is up)</li>
              <li><code>4</code>:    Ask for a pong reply to the given port (Note: sending a pong is currently not implemented by foghorn)</li>
              <li><code>255</code>:  Get list of endpoints and their states (Note: if there are no endpoints known, foghorn will not reply, resulting in a 404 error)</li>
            </ul>
          </li>
          <li><code>getlist</code>: alias for <code>cmd=255</code>, no value required</li>
          <li><code>machine</code>: activate JSON output mode, no value required</li>
        </ul>

        <p>When using <code>getlist</code> or <code>cmd=255</code>, the following parameters can be used:</p>
        <ul>
          <li><code>start</code>: Start listing entries from this index - default is <code>0</code></li>
          <li><code>end</code>:   Stop listing entries after this index - default is <code>1</code> (Note: entries will be listed until the UDP packet it is being sent in is full)</li>
        </ul>

        <p>If the foghorn instance could not be reached (before the timeout), the page will return a 404 status code. Otherwise it will return 200 as normal.<br>
        When retrieving the list of endpoints, this page will listen for foghorn's replies until it has received all entries it has asked for. If it has received some (but not all) at the timeout, it will list the messages it did receive as well as return a 404 code.</p>
      `);
      res.end();
      return;
    }
    console.log("Contacting host:", host, " port:", port, " passphrase:", passphrase);


    let messages = [];
    let request = {};

    let ret = await new Promise((resolve, reject) => {
        //UDP Socket magic
        //Foghorn requires at least 21 bytes to get to the data part. Then we need to add the size of the passphrase
        //Create the full message meant to be sent to Foghorn so we can use that to determine the SHA256 phrase!
        //console.log(name, "lenght: ",name.length);
      
        function createUDPMessage(command = 0,data = []) {
          //LETS MAKE THAT UDP
          //GO TO FOGHORN DOCUMENTATION FOR HOW TO BYTE

          arraySize = (21 + data.length);
          var message = new Uint8Array(arraySize);
          encodeIntoAtPosition("FOGH", message, 0);

          message[20] = command; //COMMAND
          if (data.length) {
            message.set(data,21);
          }

          //console.log(message);// Check array so far

          //SHA256 magic sauce
          //Create the UDP message first so we know the contents for the SHA256!
          hashmeer = createHash('sha256');
          hashmeer.update(message.slice(20));
          hashmeer.update(passphrase);

          message.set(hashmeer.digest().slice(0, 16), 4);

          return message;
        }

        let data;

        switch (command) {
          case 0: {
            //Announce public port mapping
            //Purpose: Request being indexed and being told public address.
            //Data:
            //  1 byte  len, len bytes protocol name
            //  2 bytes len, len bytes mapping name
            let protname = "health";
            data = new Uint8Array(1+protname.length+2+mapname.length);
            data[0] = protname.length;
            encodeIntoAtPosition(protname,data,1);
            data.set(intToByteArray(mapname.length,2),1+protname.length);
            encodeIntoAtPosition(mapname,data,3+protname.length);
            request = {
              type: "Announce public port mapping",
              protocol: protname,
              mapping_name: mapname
            };
            break;
          }
          case 4: {
            //Ping
            //Purpose: Ask for a pong reply to the given port with the same nonce
            //Data:
            //  2 bytes port
            //  remaining bytes “nonce”
            data = new Uint8Array(4);
            let port = 0;
            let nonce = randomBetween(0,65536);
            data.set(intToByteArray(port,2),0);
            data.set(intToByteArray(nonce,2),2);
            request = {
              type: "Ping",
              port: port,
              nonce: nonce
            };

            break;
          }
          case 255: {
            //0xFF = Get list of endpoints
            //Purpose: Retrieve list of known endpoints, paginated
            //Data:
            //  2 bytes entry number start
            //  2 bytes entry number end (optional, == start number if left out): 
            //    NOTE: foghorn will always return the maximum entries that will fix in a packet

            let start = 0;
            let end = null;

            if (url.searchParams) {
              if (url.searchParams.has("start")) { start = Number(url.searchParams.get("start")); }
              if (url.searchParams.has("end")) { end = Number(url.searchParams.get("end")); }
            }

            data = new Uint8Array(2+(end === null ? 0 : 2));
            data.set(intToByteArray(start,2),0);
            if (end !== null) {
              data.set(intToByteArray(end,2),2);
            }
            request = {
              type: "Get list of endpoints",
              start: start,
              end: end
            };

            break;
          }
        }
        
        let message = createUDPMessage(command,data);
        request.command = command;
        request.data = data.toString();

        let sentTime = process.hrtime();

        console.log("Sending",request);

        let client = dgram.createSocket('udp4');

        console.log(host+":"+port,"Sending:",new TextDecoder().decode(message));
        //console.log("command:",command,"data:",data,"message:",message);
        //UDP message to send
        let to = setTimeout(() => {
            client.close();
            resolve(false);
        }, 2500);
        client.on('message', function(message, remote) {
            try {
              console.log(remote.address + ':' + remote.port + ' Received: ' + message);

              let m = new Uint8Array(message);

              if (Buffer.from(m.subarray(0,4)).toString() != "FOGH" ) {
                console.warn("Reply is not in foghorn format."); 
                return;
              }

              let command = m[20];
              let data = message.subarray(21);
              let out = {
                command: command
              };

              switch (command) {
                case 1: { 
                  out.type = "Mapping response";
                  out.statebyte = data[0];
                  out.state = HRstate(data[0]);
                  out.port = data[1]*256+data[2];
                  out.ip = HRip(data.subarray(3));
                  break;
                }
                case 5: {
                  out.type = "Pong";
                  out.nonce = arrayToInt(data);
                  out.request_nonce = request.nonce;
                  endTime = process.hrtime(sentTime);
                  out.delay_ms = endTime[0] * 1000 + endTime[1] / 1000000;
                  break;
                }
                case 8: {
                  out.type = "Consistency checking address";
                  out.address = Buffer.from(data).toString();
                  break;
                }
                case 254: {
                  out.type = "Endpoint list response";

                  //Purpose: Returns a page of entries
                  //Data:
                  //  2 bytes first entry in this message number
                  //  2 bytes total entries in system (not the number in this message!)

                  out.first = arrayToInt(data.subarray(0,2));
                  out.total = arrayToInt(data.subarray(2,4));
                  out.entries = [];
                  //out.data = data.toString();

                  let pos = 4;
                  while (pos < data.length) {

                    //one or more consecutive entries:
                    //  1 byte entry length (excluding entry length itself!)
                    //  1 byte len, len bytes mapping name (truncated to max 100 chars)
                    //  1 byte len, len bytes protocol name (truncated to max 10 chars)
                    //  1 byte state; 0x00 = open port, 0x01 = consistent port, 0x02 = predictable port, 0xfe = impenetrable, 0xff = undetermined
                    //  2 bytes port
                    //  4 or 16 bytes IP (raw binary, 4 bytes for IPv4, 16 bytes for IPv6)

                    let entry = {};
                    let entry_length = data[pos]+1;
                    let n = 1;
                    let l = data[pos+n];
                    n++;
                    entry.mapname = Buffer.from(data.subarray(pos+n,pos+n+l)).toString();
                    n += l;
                    l = data[pos+n];
                    n++;
                    entry.protocol = Buffer.from(data.subarray(pos+n,pos+n+l)).toString();
                    n += l;
                    entry.statebyte = data[pos+n];
                    entry.state = HRstate(entry.statebyte);
                    n++;
                    entry.port = arrayToInt(data.subarray(pos+n,pos+n+2));
                    n += 2;
                    entry.ip = HRip(data.subarray(pos+n,pos+entry_length)); //the remainder should be the ip (4 or 16 bytes)

                    //entry.length = entry_length;
                    //entry.n = n;

                    out.entries.push(entry);
                    pos += entry_length;
                  }

                  break;
                }
              }

              console.log("Received message:",out);
              messages.push(out);


              if (request.command == 255) { //get list
                //there may be more packages coming. Wait for them!
                
                //to test multiple packages, use something like this in MI dev console:
                //a = {connector:"TSSRT",port:8889}; for (var i = 0; i < 50; i++) { a.foghorn = ["patchy.ddvtech.com/WHEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE_"+i]; a.port++; mist.send(console.warn,{addprotocol:$.extend({},a)}); }

                if (request.end === null || (out.first + out.entries.length >= request.end) || (out.first + out.entries.length >= out.total)) {
                  //done!
                  clearTimeout(to);
                  resolve(true);
                }

              }
              else {
                clearTimeout(to);
                resolve(true);
              }

            }
            catch(e) { 
              console.warn(e); 
              resolve(false); 
              clearTimeout(to); 
            }


        });

        client.send(message, 0, message.length, port, host, function(err, bytes) {
          //console.log('UDP message sent to ' + host + ':' + port);
          if (err) {
            clearTimeout(to);
            client.close();
            resolve(false);
          }
        });

    });


    res.writeHead(ret ? 200 : 404, {
      'Access-Control-Allow-Origin': "*",
      "Content-Type": (mode == "human" ? "text/plain;charset=UTF8" : "application/json")
    });

    if (mode == "human") {
      if (ret) {
        res.write("\u2705 " + host + ":" + port + " is responding\n");
      }
      else {
        res.write("\u274C Timeout or error from " + host + ":" + port + " with passphrase '" + passphrase + "'\n");
      }
      if (messages.length) {
        res.write("Received "+messages.length+" message(s):\n");
        res.write(JSON.stringify(messages,null,2));
        //res.write("<script>console.log("+JSON.stringify(messages)+");</script>");
      }
    }
    else {
      //machine mode
      res.write(JSON.stringify({
        host: host,
        port: port,
        passphrase: passphrase,
        request: request,
        responses: messages
      }));

    }

    res.end(); //end the HTTP response

}).listen(hport); //the server object listens on hport

console.log("Nautophone ready!\nReach me at http://localhost:"+hport+"/\n")
