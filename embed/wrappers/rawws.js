mistplayers.rawws = {
  name: "WebCodec Player",
  mimes: ["ws/video/raw"],
  priority: MistUtil.object.keys(mistplayers).length + 1,
  isMimeSupported: function (mimetype) {
    return (MistUtil.array.indexOf(this.mimes,mimetype) == -1 ? false : true);
  },
  isBrowserSupported: function (mimetype,source,MistVideo) {


    if (!("WebSocket" in window) || (MistVideo.info.capa && !MistVideo.info.capa.ssl)) {
      MistVideo.log("This player requires websocket support");
      return false;
    }
    if (!window.VideoDecoder || !window.AudioDecoder) {
      //unavailable at http
      return false; 
    }
    if (!window.Worker) return false;

    //check for http/https mismatch
    if (location.protocol != MistUtil.http.url.split(source.url.replace(/^ws/,"http")).protocol) {
      if ((location.protocol == "file:") && (MistUtil.http.url.split(source.url.replace(/^ws/,"http")).protocol == "http:")) {
        MistVideo.log("This page was loaded over file://, the player might not behave as intended.");
      }
      else {
        MistVideo.log("HTTP/HTTPS mismatch for this source");
        return false;
      }
    }
    //we can actually test if track codecs will work.. but only in a promise
    //if we have them cached, good! we can return that. otherwise..
    //for now, assume all codecs are available
    //we will perform the actual testing in player buildup and nextCombo if it's bad
    var cache = {};
    if (this.cacheSupported && (MistVideo.stream in this.cacheSupported)) {
      cache = this.cacheSupported[MistVideo.stream];
    }
    else {
      this.cacheSupported[MistVideo.stream] = {};
    }
    if (MistVideo.info && MistVideo.info.meta && MistVideo.info.meta.tracks) {
      var playabletracks = {};
      for (var i in MistVideo.info.meta.tracks) {
        var track = MistVideo.info.meta.tracks[i];
        switch (track.type) {
          case "audio":
          case "video": {
            if (i in cache) {
              if (cache[i]) playabletracks[track.type] = true;
            }
            else {
              //we haven't actually tested yet if this track will play - but assume it will
              playabletracks[track.type] = "maybe";
            }
            break;
          }
          case "meta": {
            if (track.codec == "subtitle") playabletracks.subtitle = true;
            break;
          }
        }
      }
      this.cacheSupported[MistVideo.stream].last = playabletracks;
      return Object.keys(playabletracks);
    }
    return []; //there are no tracks in the metadata..
  },
  player: function(){
    this.onreadylist = [];
  },
  cacheSupported: {
    //<streamname>: { last: <playabletracks>, trackId: true/false }
  },
  isTrackSupported: function(track){
    function str2bin(str) {
      //the init data received is a string, because it is read from the stream info json
      //it is raw asci, except where the value could not be encoded as asci, where it is the escaped char code instead
      var out = new Uint8Array(str.length);
      for (i = 0; i < str.length; i++) {
        out[i] = str.charCodeAt(i);
      }
      return out;
    }

    var decoder;
    var config = {
      codec: track.codecstring ? track.codecstring : (""+track.codec).toLowerCase()
    }
    if (track.init != "") {
      config.description = str2bin(track.init);
    }
    switch (track.type) {
      case "video": {
        if (track.codec == "JPEG") {
          decoder = ImageDecoder;
          decoder.isConfigSupported = function(){
            return new Promise(function(resolve,reject){
              decoder.isTypeSupported("image/jpeg").then(function(isSupported){
                resolve({
                  supported: isSupported,
                  config: { codec: "image/jpeg" }
                });
              }).catch(reject);
            }) 
          };
        }
        else decoder = VideoDecoder;
        break;
      }
      case "audio": {
        //TODO why does it tin-can sometimes? can the polyfill help?
        //^ seems to fix temporarily when calling pipeline.empty() ?!
        decoder = AudioDecoder;
        config.numberOfChannels = track.channels;
        config.sampleRate = track.rate;
        break;
      }
      default: {
        return false;
      }
    }
    return new Promise(function(resolve,reject){
      decoder.isConfigSupported(config).then(function(result){
        //if (MistVideo.) console.warn("WebCodecs player: isConfigSupported?",result.supported,result,result.config.description,track);
        resolve(result);
      }).catch(reject);
    });
  }
};
var p = mistplayers.rawws.player;
p.prototype = new MistPlayer();
p.prototype.build = function (MistVideo,callback) {
  var main = this;

  var video = document.createElement("video");
  this.setSize = function(size){
    video.style.width = size.width+"px";
    video.style.height = size.height+"px";
  };

  this.debugging = true;
  //this.debugging = "verbose"; //logs all parsed chunks
 
  function myAudioStreamTrackGenerator() {
    const ac = new AudioContext;
    const dest = ac.createMediaStreamDestination();
    const [track] = dest.stream.getAudioTracks();
    track.writable = new WritableStream({
      async start(controller) {
        this.arrays = [];
        function worklet() {
          registerProcessor("mstg-shim", class Processor extends AudioWorkletProcessor {
            constructor() {
              super();
              this.arrays = [];
              this.arrayOffset = 0;
              this.port.onmessage = ({data}) => this.arrays.push(data);
              this.emptyArray = new Float32Array(0);
            }
            process(inputs, [[output]]) {
              for (let i = 0; i < output.length; i++) {
                if (!this.array || this.arrayOffset >= this.array.length) {
                  this.array = this.arrays.shift() || this.emptyArray;
                  this.arrayOffset = 0;
                }
                output[i] = this.array[this.arrayOffset++] || 0;
              }
              return true;
            }
          });
        }
        await ac.audioWorklet.addModule(`data:text/javascript,(${worklet.toString()})()`);
        this.node = new AudioWorkletNode(ac, "mstg-shim");
        this.node.connect(dest);
        return track;
      },
      write(audioData) {
        const array = new Float32Array(audioData.numberOfFrames * audioData.numberOfChannels);
        audioData.copyTo(array, {planeIndex: 0});
        this.node.port.postMessage(array, [array.buffer]);
        audioData.close();
      }
    });
    return track;
  }
  if (!window.MediaStreamTrackGenerator) {
    // Polyfill by Jan-Ivar Bruaroey, via https://blog.mozilla.org/webrtc/unbundling-mediastreamtrackprocessor-and-videotrackgenerator/
    // atm (Nov 2025) Chrome doesn't need it, Firefox/Safari does
    window.MediaStreamTrackGenerator = class MediaStreamTrackGenerator {
      constructor({kind}) {
        if (kind == "video") {
          const canvas = document.createElement("canvas");
          const ctx = canvas.getContext('2d', {desynchronized: true});
          const track = canvas.captureStream().getVideoTracks()[0];
          track.writable = new WritableStream({
            write(frame) {
              canvas.width = frame.displayWidth;
              canvas.height = frame.displayHeight;
              ctx.drawImage(frame, 0, 0, canvas.width, canvas.height);
              frame.close();
            }
          });
          return track;
        }
        else if (kind == "audio") {
          const ac = new AudioContext;
          const dest = ac.createMediaStreamDestination();
          const [track] = dest.stream.getAudioTracks();
          track.writable = new WritableStream({
            start(controller) {
              var me = this;
              this.arrays = [];
              function worklet() {
                registerProcessor("mstg-shim", class Processor extends AudioWorkletProcessor {
                  constructor() {
                    super();
                    this.arrays = [];
                    this.arrayOffset = 0;
                    this.port.onmessage = ({data}) => this.arrays.push(data);
                    this.emptyArray = new Float32Array(0);
                  }
                  process(inputs, [[output]]) {
                    for (let i = 0; i < output.length; i++) {
                      if (!this.array || this.arrayOffset >= this.array.length) {
                        this.array = this.arrays.shift() || this.emptyArray;
                        this.arrayOffset = 0;
                      }
                      output[i] = this.array[this.arrayOffset++] || 0;
                    }
                    return true;
                  }
                });
              }
              return ac.audioWorklet.addModule(`data:text/javascript,(${worklet.toString()})()`).then(function(){
                me.node = new AudioWorkletNode(ac, "mstg-shim");
                me.node.connect(dest);
                return track;
              });
            },
            write(audioData) {
              const array = new Float32Array(audioData.numberOfFrames * audioData.numberOfChannels);
              audioData.copyTo(array, {planeIndex: 0});
              this.node.port.postMessage(array, [array.buffer]);
              audioData.close();
            }
          });
          return track;
        }
      }
    };
  }
  
  var supportedCodecs = [];

  /*
  function WorkerTimeout() {
    var workerHandler = `
      self.timers = {};
      self.onmessage = function(e){
        const [uid,delay] = e.data.split(" ");
        if (delay == "clear") {
          if (uid in self.timers) {
            self.clearTimeout(self.timers[uid]);
            delete self.timers[uid];
          }
        }
        else {
          self.timers[uid] = self.setTimeout(function(){
            self.postMessage(uid);
            delete self.timers[uid];
          },delay);
        }
      }
    `;
    var worker = new Worker(
      window.URL.createObjectURL(
        new Blob([`self.onmessage=${workerHandler}`], {type: 'application/javascript'})
      )
    );
    var nextuid = 1;
    var timers = {};
    worker.onmessage = function(e){
      const uid = e.data;
      if (uid in timers) {
        timers[uid]();
      }
    };
    this.set = function(callback,delay){
      var uid = nextuid;
      nextuid++;
      worker.postMessage([uid,delay].join(" "));
      timers[uid] = callback;
      return uid;
    };
    this.clear = function(uid){
      if (uid in timers) {
        worker.postMessage([uid,"clear"].join(" "));
        delete timers[uid];
      }
    };
    this.destroy = function(){
      worker.terminate();
    };
  }
  var myTimeout = new WorkerTimeout();
  /*
  setTimeout(function(){
    var now = performance.now();
    var tid = myTimeout.set(function(){
      console.log("hello! delay was",performance.now() - now,"ms");
    },2e3);
    
    setTimeout(function(){
      myTimeout.clear(tid);
    },1e3);

  },5e3)*/

  /*function CompensatingTimeout() {
    var deviation = 0;
    var deviations = [];

    function average(array) {
      return array.reduce(function(partialsum,a){ return partialsum + a; }) / array.length;
    }

    function addDiff(diff) {
      deviations.push(diff);
      if (deviations.length > 8) deviations.shift(); 
      deviation = average(deviations);
    }

    this.set = function(callback,delay){
      var target = performance.now()+delay;
      return setTimeout(function(){
        var diff = target - performance.now();
        //console.log("Timeout deviation:",diff,"ms Deviation used:",deviation,"ms");
        addDiff(diff);
        callback();
      },delay+deviation);
    };
    this.getDeviation = function(){
      return deviation;
    };
  }*/
  //var myTimeout = new CompensatingTimeout();
  /*function whileTrueTimeout() {
    var queue = [];
    var uid = 0;
    var looping = false;
    var myTimeout = this;
    this.check = function(timerObj){
      var now = performance.now();
      if (timerObj.time <= now + 1) {
        //console.log(now,">",timerObj.time,"late:",now-timerObj.time);
        //setTimeout(function(){
          timerObj.callback();
        //},0);
        return true;
      }
      return false;
    };
    this.loop = function(){
      looping = true;
      while (queue.length && this.check(queue[0])) {
        queue.shift();
      }
      if (queue.length) {
        setTimeout(function(){
          myTimeout.loop();
        },Math.min(5,queue[0].time - performance.now()));
        return;
      }
      looping = false;
    };
    this.set = function(callback,delay){
      //create timer object
      var timerObj = {
        time: performance.now() + delay,
        callback: callback,
        uid: uid
      };
      uid += 1;

      function insert() {
        //insert into the queue, sorted by time
        for (var i = 0; i < queue.length; i++) {
          if (queue[i].time > timerObj.time) {
            queue.splice(i,0,timerObj);
            return;
          }
        }
        queue.push(timerObj);
      }

      insert();

      if (!looping) this.loop();

      return uid;
    };
    this.clear = function(uid){
      console.warn("clear timer",uid);
      for (var j = 0; j < queue.length; j++) {
        if (queue[j].uid == uid) {
          queue.splice(j,1);
          return true;
        }
      }
      return false;
    };
  }*/
  //myTimeout = new whileTrueTimeout();
  //this.myTimeout = myTimeout;

  function RawPlayer(url) {
    var controller = this;
    this.connecting = false;

    this.createControlChannel = function() {
      this.control = new MistUtil.shared.ControlChannel(function(){
        var ws = new WebSocket(url);
        ws.binaryType = "arraybuffer";
        return ws;
      },MistVideo);
      this.connection = this.control;

      this.control.addListener("channel_timeout").then(function(){
        MistVideo.log("Control channel timeout - try next combo","error");
        MistVideo.nextCombo(undefined,"control channel timeout");
      });
      this.control.addListener("channel_error").then(function(){
        if (controller.control.was_connected) {
          MistVideo.log("Attempting to reconnect control channel");
          controller.createControlChannel();
        }
      });
      //live passthrough of the debugging flag
      Object.defineProperty(this.control,"debugging",{
        get: function(){
          return main.debugging; 
        }
      });
      return this.control;
    }
    this.control = this.createControlChannel();

    function RawChunk(data) {
      /* infoBytes:
       * start - length - meaning
       * 0       1        track index
       * 1       1        if == 1: keyframe 2: init data else (0): normal frame
       * 2       8        (uint) timestamp (when frame should be sent to decoder) [ms]
       * 10      2        (int)  offset (when frame should be outputted compared to timestamp) [ms]
       * */


      var l = 12; //length of the info bytes at the beginning of the data
      var info = new Uint8Array(data.slice(0,l)); //the info bytes

      this.data = new Uint8Array(data.slice(l,data.byteLength)); //the actual data

      this.track = info[0];
      this.type = function(b) {
        switch (b) {
          case 1: return "key";
          case 2: return "init";
          default: return "delta";
        }
      }(info[1]);
      this.timestamp = function(infoBytes) {
        var v = new DataView(infoBytes.slice(2,10).buffer);
        return Number(v.getBigUint64()); //getBigUint64 returns a big int, but while that is < 2^53 - 1, it can be converted to a "normal" Number
        //return v.getUint32(4); //64 bit hard, but we can settle for a 32bit integer that rolls over
      }(info);


      this.offset = function(infoBytes) {
        //return infoBytes[10]*256+infoBytes[11];
        return new DataView(infoBytes.slice(10,12).buffer).getInt16(); 
      }(info);
      
      if (main.debugging == "verbose") console.log("Parsed binary data received from WS:",MistUtil.format.time((this.timestamp + this.offset)*1e-3,{ms:true}),this);

    }
    function getTrack(trackIndex) {
      if (!MistVideo.info || !MistVideo.info.meta || !MistVideo.info.meta.tracks) return;
      for (var i in MistVideo.info.meta.tracks) {
        var track = MistVideo.info.meta.tracks[i];
        if (track.idx == trackIndex) {
          return track;
        }
      }
      throw "Missing metadata for track "+trackIndex;
    }

    this.data_queue = [];

    function JitterTracker(){
      let chunks = [];    //sliding window with data of 8 chunks (timestamp and received time only)
      let speed = 1;      //requested download speed
      let lastcalced = 0; //when the last peak was added to the log 
      let curJitter = 0;  //maximum jitter calculted during the last second
      let peaks = [];     //sliding window of previous 8 measurements of curJitter
      let maxJitter = 120; //intialize at 120ms, will change based on measured jitter (increase immediately, decrease slowly)
      this.addChunk = function(chunk) { //add all chunks
        chunks.push([
          performance.now(),
          chunk.timestamp
        ]);
        if (chunks.length > 8) chunks.shift();

        let jitter = this.calcJitter(); //calculate the jitter
        if (jitter > curJitter) curJitter = jitter; //curjitter is the max

        if (performance.now() > lastcalced + 1e3) { //every second, save the current peak jitter and reset
          peaks.push(curJitter);
          if (peaks.length > 8) peaks.shift();

          let maxPeak = Math.max.apply(null,peaks);
          let avgPeak = peaks.reduce(function(partialsum,a){ return partialsum + a; }) / peaks.length;
          let weighted = (avgPeak + maxPeak * 2) / 3 + 1;
          //limit lowering to max 250ms per step
          if (maxJitter > weighted + 500) { weighted = maxJitter - 500; }
          maxJitter = (maxJitter + weighted) / 2;

          curJitter = 0;
          lastcalced = performance.now();
        }
      }
      this.calcJitter = function(){ //calculate jitter based on the last 8 packets (if available)
        if (chunks.length <= 1) return 0;
        if (speed == "fast_forward") return 0;
        let oldest = chunks[0];
        let newest = chunks[chunks.length-1];
        let clock_time_passed = newest[0] - oldest[0]; //[ms]
        let media_time_passed = newest[1] - oldest[1]; //[ms]
        let jitter = media_time_passed/speed - clock_time_passed; //[ms]
        return Math.max(0,jitter);
      };
      this.get = function(){
        return maxJitter;
      }
      this.setSpeed = function(newspeed){
        if (speed != newspeed) {
          if (newspeed == "auto") newspeed = 1;
          speed = newspeed;
          this.reset();
        }
      };
      this.reset = function(){
        chunks = [];
      }
    }
    this.jitter = new JitterTracker();

    //handles incoming binary data (from the control channel) and inserts it into the appropriate pipeline
    this.receiver = function(data) {
      //var splitData = splitToInfoAndChunk(data);
      var chunk = new RawChunk(data)

      if (!(chunk.track in controller.pipelines)) {
        if (main.debugging) console.warn("Received data for track "+chunk.track+" but there is no pipeline for it, attempting to create it");
        new Pipeline(chunk.track);
      }

      var pipeline = controller.pipelines[chunk.track];


      if (chunk.type == "init") {
        pipeline.configure(chunk.data);
        return;
      }
      try {
        pipeline.receive({
          type: chunk.type,                                //Indicates if the chunk is a key chunk that does not rely on other frames for encoding. One of: "key" The data is a key chunk. "delta" The data is not a key chunk.
          timestamp: (chunk.timestamp + chunk.offset)*1e3, //An integer representing the timestamp of the video in microseconds.
          data: chunk.data,                                //An ArrayBuffer, a TypedArray, or a DataView containing the video data.
          //transfer: [chunk.data]                           //An array of ArrayBuffers that EncodedVideoChunk will detach and take ownership of. If the array contains the ArrayBuffer backing data, EncodedVideoChunk will use that buffer directly instead of copying from it.
        });
        MistUtil.event.send("progress",null,video);
      }
      catch (err) {
        if (main.debugging) console.error("Error while decoding track "+chunk.track+" ("+pipeline.track.codec+" "+pipeline.track.type+")",chunk,err);
        MistVideo.log(err+" while decoding track "+chunk.track+" ("+pipeline.track.codec+" "+pipeline.track.type+")");
        pipeline.reset();
      }

      controller.jitter.addChunk(chunk);
    }

    this.pipelines = {}; //add a decoder pipeline for each track
    function WebCodecsWorker() {
      var self = this;
      function myDedicatedWorker() {
        var self = this;
        var pipelines = {};

        function Pipeline(){
          var pipeline = this;
          this.writer = undefined;
          this.decoder = undefined;

          this.addWriter = function(writable){
            this.writer = writable.getWriter();
          };
          this.writeFrame = function(frame){
            try {
              return pipeline.writer.write(frame)
            }
            catch(err) {
              return new Promise(function(resolve,reject){
                reject(err);
              });
            }
          };
        }

        this.onmessage = function(e) {
          var msg = e.data;
          msg.status = "ok";

          if (("idx" in msg) && !(msg.idx in pipelines)) pipelines[msg.idx] = new Pipeline();
          var pipeline = pipelines[msg.idx];
          
          switch (msg.type) {
            case "writeFrame": {
              pipeline.writeFrame(msg.frame).then(function(){
                //msg.frame.close(); //shouldn't be needed
                delete msg.frame;
                self.postMessage(msg);
              }).catch(function(err){
                msg.status = "error";
                msg.error = err;
                msg.frame.close();
                delete msg.frame;
                self.postMessage(msg);
              });
              return; //do not post default reply
            }
            case "setWritable": {
              pipeline.addWriter(msg.writable);
              delete msg.writable;
              break;
            }
            case "close": {
              delete pipelines[msg.idx];
              break;
            }
          }

          this.postMessage(msg);
        }
      }
      this.worker = new Worker(URL.createObjectURL(new Blob(["("+myDedicatedWorker.toString()+")();"],{type: 'application/javascript'})));
      this.worker.onmessage = function(e){
        var msg = e.data;
        var key = [msg.type,msg.idx,msg.uid].join("_");
        if (key in self.listeners) {
          self.listeners[key](msg);
          delete self.listeners[key];
        }
        else {
          console.warn("Received msg from worker",msg);
        }
      };

      this.listeners = {};
      var uid = 0;
      this.addListener = function(opts,callback){
        var key = [opts.type,opts.idx,opts.uid].join("_");
        if (callback) {
          this.listeners[key] = callback;
        }
        var self = this;
        return new Promise(function(resolve,reject){
          self.listeners[key] = function(msg){
            if (msg.status == "error") return reject(msg);
            resolve(msg);
          };
        });
      };

      this.post = function(msg,opts){
        if (!("uid" in msg)) {
          msg.uid = uid++;
        }
        if (opts && opts.transfer && !Array.isArray(opts.transfer)){
          opts.transfer = [opts.transfer];
        }
        this.worker.postMessage(msg,opts);
        return new Promise(function(resolve,reject){
          self.addListener(msg).then(resolve).catch(reject);
        });
      };
      this.terminate = function(){
        this.worker.terminate();
      };
    }
    this.worker = new WebCodecsWorker();

    function Pipeline(track) {
      if (!(typeof track == "object")) {
        track = getTrack(track);
      }
      if ((track.type != "audio") && (track.type != "video")) {
        controller.control.send({type:"hold"})
        return; 
      }
      var pipeline = this;
      pipeline.track = track;
      pipeline.codec = track.codecstring || (""+track.codec).toLowerCase();
      pipeline.lastReset = null;
      pipeline.closeWhenEmpty = false;

      //var buffer = 500; //keep frame for additional 500ms after decoding

      MistVideo.log("Creating pipeline for track "+track.idx+" ("+track.codec+" "+track.type+")");

      /*
       
        When a new pipeline is created, the following are prepared:
        A track writer
        A track Generator
        A decoder

        The pipeline must first be "configured" (possibly requiring init data) using Pipeline.configure(initdata), after that it can process chunks.
        Pipeline.configure() also adds Pipeline.trackGenerator to controller.stream.
        
        Data travels as follows:

        - Pipeline.receive(chunkOpts): the chunk is either queued in the input queue or sent to decode
        - Pipeline.decode(chunkOpts): the chunk is converted to an EncodedVideoChunk or EncodedAudioChunk and sent to the decoder
        - Pipeline.decoder.decode(chunk): the chunk is decoded and the frame is sent to decoderOpts.output (or triggers decoderOpts.error)
        - decoderOpts.output: the frame is added to the output queue
        - processQueue: when the targetTime is reached, the frame is sent to pipeline.trackWriter, which adds it to pipeline.trackGenerator
        - Pipeline.trackGenerator is a track added to controller.stream, a MediaStream
        - controller.stream is the srcObject of the video element
       
      */
      pipeline.createTrack = function(){
        if (pipeline.trackGenerator) {
          controller.stream.removeTrack(pipeline.trackGenerator);
        }
        //if (track.type == "audio") {
        //pipeline.trackGenerator = new myAudioStreamTrackGenerator();
        //}
        //else {
        pipeline.trackGenerator = new MediaStreamTrackGenerator({kind:track.type});
        //}
        
        //pipeline.trackWriter = pipeline.trackGenerator.writable.getWriter();
        controller.worker.post({
          idx: track.idx,
          type: "setWritable",
          writable: pipeline.trackGenerator.writable
        },{transfer: pipeline.trackGenerator.writable});

        pipeline.trackWriter = new function TrackWriterWorker(){
          this.write = function(frame){
            pipeline.stats.timing.writable.start(frame.timestamp);
            return controller.worker.post({
              idx: track.idx,
              type: "writeFrame",
              frame: frame
            },{transfer: frame}).finally(function(){
              pipeline.stats.timing.writable.end(frame.timestamp);
            });
          };
        };
      };
      pipeline.createTrack();

      var outputTimer = {
        tid: false,
        targetTime: false
      };
      pipeline.waitingTimer = false;
      //check if we should process queue
      var processTime = 0;
      function processQueue(fromTimer) { //fromTimer should be true when processQueue is being called from the outputTimer - to indicate it is not late; this is the expected time to process the next frame
        if (controller.frameTiming.paused) return;
        if (!pipeline.queues.out.length) {
          if (pipeline.closeWhenEmpty) {
            pipeline.close(true);
          }
          else {
            var newBaseTime = performance.now() - controller.frameTiming.out.timestamp*1e-3 / controller.frameTiming.speed.combined;
            var inc = newBaseTime - controller.frameTiming.out.baseTime;
            if (isNaN(inc) || inc > 1e3 || inc < 50) {
              inc = 50;
            }
            if (main.debugging) {
              console.log(
                "Uhoh: output queue for "+function(t){
                  switch (t) {
                    case "video": return "🎞️";
                    case "audio": return "🎵";
                    default: return t;
                  }
                }(track.type)+" track",track.idx,"is empty!",
                "Increased playback timer with",Math.round(inc),"ms"
              );
            }
            controller.frameTiming.out.baseTime = newBaseTime;
            if (!pipeline.waitingTimer) {
              pipeline.waitingTimer = setTimeout(function(){
                //add some time to the playback timer
                MistUtil.event.send("waiting",null,video);
              },1e3);
              //the timer will be canceled
              //- when something enters the queue
              //- when on_time with changing msg.current is received
            }
          }
          return;
        }
        if (outputTimer.tid) {
          if (main.debugging && (new Date().getTime() - outputTimer.targetTime > 1e3)) {
            console.log(
              "⚠️",
              function(t){
                switch (t) {
                  case "video": return "🎞️";
                  case "audio": return "🎵";
                  default: return t;
                }
              }(track.type),
              "processQueue() outputTimer already active:",
              "ending at",new Date(outputTimer.targetTime).toLocaleTimeString(),
              "seeking to",controller.frameTiming.seeking ? MistUtil.format.time(controller.frameTiming.seeking*1e-6) : "no",
              "first timestamp",MistUtil.format.time(pipeline.queues.out[0].timestamp*1e-6)
            );
          }
          return;
        }
        if (pipeline.waitingTimer) {
          clearTimeout(pipeline.waitingTimer);
          pipeline.waitingTimer = false;
        }

        var frame = pipeline.queues.out[0];

        if ((pipeline.queues.out.length > 1) && (pipeline.queues.out[1].timestamp < frame.timestamp)) {
          if (main.debugging) {
            console.warn(
              "⚠️",
              function(t){
                switch (t) {
                  case "video": return "🎞️";
                  case "audio": return "🎵";
                  default: return t;
                }
              }(track.type),
              "Frames out of order!",
              "diff:",(pipeline.queues.out[1].timestamp - frame.timestamp)*1e-3,"ms",
              "this frame:",frame,
              "next frame:",pipeline.queues.out[1]
            );
          }
          //put frame at index 0 at index 1
          var next = pipeline.queues.out.splice(1,1)[0]; //remove and save next frame
          pipeline.queues.out.unshift(next); //put it at the front of the queue

          return processQueue(fromTimer);
        }

        var frameTiming = controller.frameTiming.get("out");
        if (frameTiming) {
          var latency = 0;//pipeline.trackGenerator.stats && pipeline.trackGenerator.stats.latency ? pipeline.trackGenerator.stats.latency : 0; //in ms
          var targetTime = frameTiming.baseTime + frame.timestamp*1e-3 / controller.frameTiming.speed.combined - latency; //in ms
          if (isNaN(targetTime)) throw "Invalid target time: "+targetTime;
          
          //var previous = controller.frameTiming.get("out");
          //var targetTime = previous.processed + (frame.timestamp - previous.timestamp)*1e-3;
          var delay = targetTime - performance.now();
          if (!fromTimer) pipeline.stats.early = delay;
          else {
            if (main.debugging && (Math.abs(delay) > 10)) {
              console.log(
                "⚠️",
                function(t){
                  switch (t) {
                    case "video": return "🎞️";
                    case "audio": return "🎵";
                    default: return t;
                  }
                }(track.type),
                "processQueue() timer end reached, but off by more than 10ms; delay:",Math.round(delay*100)/100,"ms",
                "original delay:",Math.round(fromTimer),"ms"
              );
            }
          }
          if (delay <= 2) { //technically delay should be 0 but if we set another timer it's going to be more inaccurate
            //if (main.debugging) console.log("display frame immediately",MistUtil.format.time(frame.timestamp*1e-6,{ms:true}),"last frame was",MistUtil.format.time(previous.timestamp*1e-6,{ms:true}));
            return writeFrame();
          }
          else {
            outputTimer.tid = setTimeout(function(){
            //outputTimer.tid = myTimeout.set(function(){
              outputTimer.tid = false;
              outputTimer.targetTime = false;
              processQueue(delay);
            },delay);
            outputTimer.targetTime = new Date().getTime()+delay;
            if (delay > 1e3) {
              console.warn(
                "⚠️",
                function(t){
                  switch (t) {
                    case "video": return "🎞️";
                    case "audio": return "🎵";
                    default: return t;
                  }
                }(track.type),
                "processQueue() delay high:",
                Math.round(delay),"ms",
                "ending at",new Date(outputTimer.targetTime).toLocaleTimeString(),
                "seeking to",controller.frameTiming.seeking ? MistUtil.format.time(controller.frameTiming.seeking*1e-6) : "no",
                "first timestamp",MistUtil.format.time(pipeline.queues.out[0].timestamp*1e-6)
              );
            }
            if (fromTimer && main.debugging) {
              console.warn(
                "⚠️",
                function(t){
                  switch (t) {
                    case "video": return "🎞️";
                    case "audio": return "🎵";
                    default: return t;
                  }
                }(track.type),
                "processQueue() timer re-timered, delay was ",fromTimer,", needs another ",delay,"ms"
              );
            }
            //console.log("delay",delay,new Date(new Date().getTime()+delay));
          }
          
        }
        else {
          controller.frameTiming.set("out",{
            baseTime: performance.now() - frame.timestamp*1e-3 / controller.frameTiming.speed.combined
          });
          return writeFrame();
        }
      }

      function writeFrame() {
        var frame = pipeline.queues.out.shift();
        if (controller.frameTiming.seeking) {
          if (frame.timestamp < controller.frameTiming.seeking) {
            //skip
            frame.close();
            if (main.debugging == "verbose") {
              console.log(
                "‼️",
                function(t){
                  switch (t) {
                    case "video": return "🎞️";
                    case "audio": return "🎵";
                    default: return t;
                  }
                }(track.type),
                "Skipped display of "+track.type+" frame because it is before the seek time",
                frame.timestamp,"<",controller.frameTiming.seeking
              );
            }
            return;
          }
          else {
            if (main.debugging) console.warn("Reached seek target of ["+MistUtil.format.time(controller.frameTiming.seeking*1e-6)+"]: returning to normal playback");
            controller.frameTiming.seeking = undefined;
            MistUtil.event.send("seeked",null,video);
            controller.jitter.reset();
          }
        }

        //console.log(track.type,frame.timestamp - latency*1e3,latency)

        /*if (frame.duration && (controller.frameTiming.speed.combined != 1)) {
          //doesn't work: this is a read only property
          frame.duration /= controller.frameTiming.speed.combined;
        }*/
        pipeline.stats.frames.out++;
        var promise = pipeline.trackWriter.write(frame).then(function(){
          //frame.close(); shouldnt be needed
          controller.frameTiming.set("out",{
            timestamp: frame.timestamp, //in microseconds
            processed: new Date().getTime()
          });
        }).catch(function(){
          console.warn("Trackwriter.write failed for",frame);
          frame.close();
        });
        MistUtil.event.send("timeupdate",null,video);
        setTimeout(processQueue,0); //asyncify
      }

      var decoderOpts = {
        output: function(frame){
          //It's better to make sure that the decoder output callback quickly returns (https://developer.chrome.com/docs/web-platform/best-practices/webcodecs)
          
          pipeline.stats.timing.decoder.end(frame.timestamp);
          if (flushing) { frame.close(); return; }

          //add to out queue
          pipeline.queues.out.push(frame);

          if (main.debugging == "verbose") console.log("New frame exited "+track.type+" decoder:",frame);

          //asyncify other tasks
          setTimeout(function(){
            controller.frameTiming.set("decoded",{
              timestamp: frame.timestamp,
              decoded: new Date().getTime()
            });
            processQueue();
          },0);
        },
        error: function(err){
          if (main.debugging) console.error("Decoder error:",err,"Config:",configOpts);
          MistVideo.log(MistUtil.format.ucFirst(track.type)+" decoder error: "+err+" (Track "+track.idx+")");
          pipeline.reset();
          //controller.control.send({type:"hold"});
        }
      };
      var configOpts = {
        codec: pipeline.codec,
        optimizeForLatency: MistVideo.info.type == "live"
      };

      var wantKey = false;
      pipeline.configure = function(header) {
        if (pipeline.decoder.state != "unconfigured") {
          if (
            //check if the new header is equal to the previous one: using == will only return true if the reference is equal
            function ArrayBuffersAreEqual(a,b){
              return a === b || indexedDB.cmp(a,b) === 0;
            }(header,configOpts.description)
          ){
            //nothing to do, carry on
            MistVideo.log("Received equal init data for track "+track.idx+" ("+track.codec+" "+track.type+") that was already configured: not resetting");
            return;
          }
          MistVideo.log("Received new init data for track "+track.idx+" ("+track.codec+" "+track.type+") that was already configured: resetting");
          pipeline.decoder.reset();
        }
        if (main.debugging) console.log("Configuring decoder for track",track.idx,"("+track.codec+" "+track.type+")",configOpts,header ? "Header size:" : "(No header)",header ? header.byteLength+"B" : "");

        controller.stream.addTrack(pipeline.trackGenerator);
        pipeline.decoder.configure(Object.assign(configOpts,{description:header}));
        wantKey = true;
        if (main.debugging) console.log("Configure track "+track.idx+" complete");
      }

      pipeline.decoder;
      pipeline.decode;
      switch (track.type) {
        case "video": { //ImageDecoder kinda works different
          if (track.codec == "JPEG") {
            pipeline.decode = function(chunkOpts){
              return new Promise(function(){
                var decoder = new ImageDecoder({
                  type: "image/jpeg",
                  data: chunkOpts.data
                });
                pipeline.stats.timing.decoder.start(chunkOpts.timestamp);
                decoder.decode().then(function(output){
                  if (output.complete) {
                    //create a new VideoFrame from this one, that has the correct timestamp
                    var frame = new VideoFrame(output.image,{
                      timestamp: chunkOpts.timestamp
                    });
                    decoderOpts.output(frame);
                  }
                });
              });
            };
            pipeline.configure = function(){
              controller.stream.addTrack(pipeline.trackGenerator);
              pipeline.decoder.state = "configured";
            };
            pipeline.decoder = {
              state: "unconfigured",
              close: function(){}
            };
          }
          else { //"VideoDecoder"
            pipeline.decoder = new VideoDecoder(decoderOpts);
            pipeline.decode = function (chunkOpts){
              if (wantKey) {
                if (chunkOpts.type == "key") {
                  if (main.debugging) {
                    console.log("Got a "+function(t){
                      switch (t) {
                        case "video": return "🎞️";
                        case "audio": return "🎵";
                        default: return t;
                      }
                    }(track.type)+" key!",chunkOpts);
                  }
                  wantKey = false;
                }
                else {
                  if (main.debugging) console.log("Skipped "+function(t){
                    switch (t) {
                      case "video": return "🎞️";
                      case "audio": return "🎵";
                      default: return t;
                    }
                  }(track.type)+" frame because waiting for a key",chunkOpts);
                  return;
                }
              }
              pipeline.stats.timing.decoder.start(chunkOpts.timestamp);
              return pipeline.decoder.decode(new EncodedVideoChunk(chunkOpts));
            }
          }
          break;
        }
        case "audio": {
          pipeline.decoder = new AudioDecoder(decoderOpts);
          configOpts.numberOfChannels = track.channels;
          configOpts.sampleRate = track.rate;
          pipeline.decode = function (chunkOpts){
            chunkOpts.type = "key";
            pipeline.stats.timing.decoder.start(chunkOpts.timestamp);
            return pipeline.decoder.decode(new EncodedAudioChunk(chunkOpts));
          }
          break;
        }
      }

      //resets the decoder and reconfigures it
      pipeline.reset = function(){
        if (resetting) return resetting;
        resetting = new Promise(function(resolve,reject){
          if (main.debugging) console.log("Resetting track "+track.idx+" ("+track.codec+" "+track.type+")");
          if (pipeline.decoder.state == "closed") {
            if ((pipeline.lastReset === null) || (new Date().getTime() - pipeline.lastReset.getTime() > 1e3)) {
              //return;
              if (main.debugging) console.warn("Recreating pipeline");
              controller.stream.removeTrack(pipeline.trackGenerator);
              if (pipeline.decoder.decodeQueueSize) pipeline.decoder.flush();
              //console.warn("ready frames",pipeline.queues.out.length,"frames in decoder",pipeline.decoder.decodeQueueSize,"(should be 0)");
              var oldpipe = pipeline;
              pipeline = new Pipeline(track); //does not seem to  break references
              pipeline.closeWhenEmpty = this.closeWhenEmpty;
              pipeline.lastReset = new Date();
              pipeline.configure(configOpts.description);
              pipeline.queues.out = oldpipe.queues.out;
              resolve();
            }
            else {
              MistVideo.timers.start(function(){
                pipeline.reset().then(resolve);
              },1e3);
            }
          }
          else {
            //"soft" reset
            pipeline.lastReset = new Date();
            if (flushing) {
              flushing.finally(function(){
                pipeline.createTrack()
                pipeline.decoder.reset();
                pipeline.configure(configOpts.description);
                resolve();
              });
            }
            else {
              pipeline.createTrack();
              pipeline.decoder.reset();
              pipeline.configure(configOpts.description);
              resolve();
            }
          }
        });
        resetting.then(function(){
          resetting = false;
        });
        return resetting;
      }
      //clear all queues
      pipeline.empty = function(){
        return new Promise(function(resolve,reject){
          pipeline.queues.in = [];
          if (outputTimer.tid) {
            clearTimeout(outputTimer.tid);
            outputTimer.tid = false;
          }
          if (main.debugging) {
            console.log(
              "Clearing input queue for "+function(t){
                switch (t) {
                  case "video": return "🎞️";
                  case "audio": return "🎵";
                  default: return t;
                }
              }(track.type)+" pipeline"
            );
          }
          if (flushing) return flushing;
          if (pipeline.decoder.state == "closed") {
            var frames = pipeline.queues.out;
            pipeline.queues.out = [];
            while (frames.length) {
              var first = frames.shift();
              if (first) first.close();
            }
            resolve();
          }
          else {
            flushing = pipeline.decoder.flush().catch(function(e){
              if (main.debugging) {
                console.error("‼️",function(t){
                  switch (t) {
                    case "video": return "🎞️";
                    case "audio": return "🎵";
                    default: return t;
                  }
                }(track.type),"Error while flushing:",e);
              }
            }).finally(function(){
              var frames = pipeline.queues.out;
              pipeline.queues.out = [];
              flushing = false;
              while (frames.length) {
                var first = frames.shift();
                if (first) first.close();
              }
              resolve();
            });
          }
        });
      }

      if (track.init == "") {
        //this codec does not use init data, so we can configure it right away
        pipeline.configure();
      }

      pipeline.queues = {
        in: [],
        out: []
      };
      pipeline.stats = {
        frames: { //total frames in/out
          in: 0,
          out: 0
        },
        early: 0, //the idle time in ms before the first frame in the queue should be outputted - if < 0 the frame is late
        framerate: {
          in: new Differentiate(function(){ return pipeline.stats.frames.in; }),
          out: new Differentiate(function(){ return pipeline.stats.frames.out; })
        },
        timing: {
          decoder: new FrameTracker(),
          writable: new FrameTracker()
        }
      };
      function Differentiate(get_func) {
        var lastval, lasttime;
        this.get = function(){
          var newval = get_func();
          var now = performance.now();
          var out;
          if (typeof lastval != "undefined") {
            var dx = newval - lastval;
            var dt = now - lasttime; //[ms]
            dt *= 1e-3; //[s] 
            out = dx/dt;
          }
          lastval = newval;
          lasttime = now;
          return out;
        }
      };
      function FrameTracker() {
        var pending = {};
        var complete = [];
        this.last_in = undefined;
        this.last_out = undefined;
        this.shift_array = [];
        
        this.start = function(timestamp){
          this.last_in = timestamp;
          pending[timestamp] = performance.now();
        };
        this.end = function(timestamp){
          this.last_out = timestamp;
          var shift = 0;
          if (!(timestamp in pending)) {
            //the timestamp has changed: assume first in, first out
            timestamp = function(a){
              for (var i in a) return i;
              return undefined;
            }(pending);
            if (timestamp === undefined) return;
            shift = timestamp - this.last_out;
          }
          complete.push(performance.now() - pending[timestamp]);
          this.shift_array.push(shift);
          delete pending[timestamp];

          while (complete.length > 16) {
            complete.shift();
          }
          while (this.shift_array.length > 8) {
            this.shift_array.shift();
          }
        };
        Object.defineProperty(this,"delay",{
          get: function(){
            if (complete.length) {
              return complete.reduce(function(partialsum,a){ return partialsum + a; }) / complete.length; //[ms]
            }
            return undefined;
          }
        });
        Object.defineProperty(this,"delta",{
          get: function(){
            if (this.last_out !== undefined) return this.last_in - this.last_out; //[microseconds]
            return undefined;
          }
        });
        Object.defineProperty(this,"shift",{
          get: function(){
            if (this.shift_array.length) {
              return this.shift_array.reduce(function(partialsum,a){ return partialsum + a; }) / this.shift_array.length; //[microseconds]
            }
            return undefined;
          }
        });
      }

      var processingQueue = false;
      var flushing = false;
      var resetting = false;
      pipeline.receive = function(chunkOpts){

        if (resetting || flushing) {
          pipeline.queues.in.push(chunkOpts);
          if (main.debugging == "verbose") {
            console.log(
              "Queued",
              function(t){
                switch (t) {
                  case "video": return "🎞️";
                  case "audio": return "🎵";
                  default: return t;
                }
              }(track.type),
              "frame because "+(resetting ? "resetting" : "flushing"),
              "Queue length:",pipeline.queues.in.length,
              chunkOpts
            );
          }
          return;
        }

        /*
        // for testing: create corrupted video data to crash the decoder
        if ((track.type == "video") && (chunkOpts.type == "delta") && (Math.random() < 0.05)) {
          chunkOpts.data[3] = Math.round(Math.random()*10);
        }
        */

        main.controller.frameTiming.set("in",{
          timestamp: chunkOpts.timestamp,
          received: new Date().getTime()
        });
        pipeline.stats.frames.in++;

        switch (pipeline.decoder.state) {
          case "configured": {
            pipeline.queues.in.push(chunkOpts);
            if (!processingQueue) {
              processingQueue = true;
 
              
              var keepone = false; //always keep one frame in the queue, so duration can be calculated
              if (keepone) {
                while (pipeline.queues.in.length >= 2) {
                  var first = pipeline.queues.in.shift();
                  var next = pipeline.queues.in[0];
                  //first.duration = 16683;
                  //first.duration = Math.max(1,next.timestamp - first.timestamp);
                  //if (first.timestamp > next.timestamp) console.warn("Negative duration",first,next);
                  pipeline.decode(first);
                }
              }
              
              else {
                while (pipeline.queues.in.length) {
                  var first = pipeline.queues.in.shift();
                  pipeline.decode(first);
                }
              }

              processingQueue = false;
            }

            return;
          }
          case "closed": { return; }
          case "unconfigured": {
            pipeline.queues.in.push(chunkOpts);
            return;
          }
          default: {
            console.log("Decoder state for ",codec,"is",pipeline.decoder.state);
            return;
          }
        }
      };

      var closingPromise = false;
      pipeline.close = function(waitEmpty){
        if (!closingPromise) closingPromise = Promise.withResolvers();
        function isEmpty() {
          if (pipeline.queues.out.length) return false;
          if (pipeline.queues.in.length) return false;
          if (pipeline.decoder.decodeQueueSize) return false;
          return true;
        }
        if (waitEmpty && !isEmpty()) {
          if (!pipeline.closeWhenEmpty) {
            pipeline.closeWhenEmpty = true;
            MistVideo.log("The pipeline for track "+track.idx+" will close once empty");
          }
        }
        else {
          controller.stream.removeTrack(pipeline.trackGenerator);
          controller.worker.post({
            type: "close",
            idx: track.idx
          });
          pipeline.decoder.close();
          pipeline.empty();
          delete controller.pipelines[track.idx];
          MistVideo.log("The pipeline for track "+track.idx+" has now been closed");
          closingPromise.resolve();
        }
        return closingPromise.promise;
      }

      controller.pipelines[track.idx] = pipeline;
    }

    //This class saves times chunks/frames were in various places in the pipelines.
    //This is used by the pipelines to determine when a frame should be outputted.
    function FrameTiming() {
      this.set = function(kind,values){
        var o = this[kind];
        if (!o) {
          this[kind] = {};
          o = this[kind];
        }
        o = Object.assign(o,values);
        return o;
      };
      this.tweakSpeed = function(tweak){
        this.setSpeed(this.speed.main,tweak);
      };
      this.setSpeed = function(speed,tweak) {
        if (!tweak) tweak = this.speed.tweak;

        if ((speed == this.speed.main) && (tweak == this.speed.tweak)) return; //nothing to do

        var combinedSpeed = speed*tweak;

        //a frames target output time is determined by adding its media timestamp (divided by the speed) to the basetime
        //if the desired speed changes, the basetime should be recalculated
        var prev = this.get("out");
        if (prev.baseTime && prev.timestamp) {
          this.set("out",{
            baseTime: prev.baseTime + prev.timestamp*1e-3 / this.speed.combined - prev.timestamp*1e-3 / combinedSpeed
          });
        }
        MistVideo.video.playbackRate = combinedSpeed;
        for (var i in controller.pipelines) {
          var p = controller.pipelines[i];
          if (p.track.type == "audio") {
            p.reset();
          }
        }

        if (this.speed.main != speed) MistUtil.event.send("ratechange",null,MistVideo.video);
        this.speed.main = speed;
        this.speed.tweak = tweak;
        this.speed.combined = combinedSpeed;
      };
      this.get = function(kind) {
        return this[kind];
      };
      this.reset = function(){
        //this.server = false; //don't reset: keep last on_time message
        this.in = false;
        this.decoded = false;
        this.out = false;
        this.paused = false;
        this.speed = {
          main: 1,
          tweak: 1,
          combined: 1
        };
      };
      this.reset();
    }
    this.frameTiming = new FrameTiming();

    var keepAway = MistVideo.info.type == "live" ? 100 : 500;
    this.desiredBuffer = function(){
      return Math.round(keepAway + controller.control.serverDelay.get() + controller.jitter.get());
    };

    //connects to the control websocket and requests codecs
    this.connect = function(){
      if (this.connecting) {
        return this.connecting;
      }

      this.connecting = new Promise(function(resolve,reject){
        //controller.createControlChannel();
        var control = controller.control;
        if ((control.connectionState == "closing") || (control.connectionState == "closed")) {
          control.reconnect();
        }

        controller.stream = new MediaStream();
        controller.stream.onaddtrack = function(e){
          if (main.debugging) console.log("MediaStream received new track",e);
        };
        video.srcObject = controller.stream;
        controller.frameTiming.reset();

        return controller.control.addListener("channel_open").then(function(){
          var codecs = [Object.values(supportedCodecs).filter(function(a){
            return a.length
          })]; //request a combination of 1 audio track and 1 video track
          //codecs = [[["H264","JPEG"]]];
          control.send({type:"request_codec_data",supported_combinations:codecs});
          //control.send({type:"request_codec_data",supportedCodecs:["H264"]});
          MistVideo.log("Requesting codecs: "+codecs.join(", "));
          controller.connecting = false;
          return control.addListener("codec_data");
        }).then(function(msg){
          if (!msg.codecs || !msg.codecs.length) {
            throw("No playable codecs available");
          }

          //received codec data, set up track pipelines
          for (var i in msg.tracks) {
            var trackIndex = msg.tracks[i];
            if (!(i in msg.codecs)) throw "No codec data for track "+trackIndex;
            controller.pipelines[trackIndex] = new Pipeline(trackIndex);
          }

          control.send({type:"play"});
          control.addListener("binary",controller.receiver);

          var requestingMoreBuffer = false;
          control.addListener("on_time",function(msg){
            //does the message mention any tracks we dont have a pipeline for?
            for (var i in msg.tracks) {
              var tid = msg.tracks[i];
              if (!(tid in controller.pipelines)) {
                //we don't have a pipeline for this track yet - set it up
                if (main.debugging) console.warn("Received on_time with track "+tid+" but there is no pipeline for it, attempting to create it");
                var p = new Pipeline(tid);
              }
            }
            //are there any pipelines that don't have a track?
            //close the pipeline when the display queue empties
            for (var tid in controller.pipelines) {
              if (msg.tracks.indexOf(Number(tid)) < 0) {
                controller.pipelines[tid].close(true);
              }
            }

            //does the current time change, and do we have pipelines with a waiting timer? reset those
            var last = controller.frameTiming.get("server");
            if (last && (msg.current != last.current)) {
              for (var tid in controller.pipelines) {
                var pipeline = controller.pipelines[tid];
                if (pipeline.waitingTimer) {
                  clearTimeout(pipeline.waitingTimer);
                  pipeline.waitingTimer = false;
                }
              }
            }

            //buffer management
            var bounds = { // if the buffer <> desiredBuffer*bounds[low,high], take action
              low: 0.6,
              high: 2
            };
            var tweaks = { //action to take when the bounds are reached
              faster: 1.05,
              slower: 0.98
            }
            var buffer = null;
            var playbackPoint = controller.frameTiming.get("out");
            var receivePoint = controller.frameTiming.get("in");
            var decodePoint = controller.frameTiming.get("decoded");
            if (playbackPoint.timestamp && decodePoint.timestamp) { 
              buffer = Math.round(decodePoint.timestamp*1e-3 - playbackPoint.timestamp*1e-3); //in ms
              decodingTime = Math.round(receivePoint.timestamp*1e-3 - decodePoint.timestamp*1e-3); //in ms
            }

            /*
            for (var i in controller.pipelines) {
              var p = controller.pipelines[i];
              var shift = p.stats.timing.decoder.shift;
              if (Math.abs(shift) > 1000) {
                p.reset();
              }
            }*/

            if ((buffer !== null) && !controller.frameTiming.seeking && !requestingMoreBuffer) { //if the buffer is known, and we're not in the middle of a seek or additional data request
              var desiredBuffer = controller.desiredBuffer();

              if (main.debugging) {
                var args = [
                  "Buffer:",
                  function(){
                    if (buffer > desiredBuffer*bounds.high) { return "⬆️"; }
                    if (buffer < desiredBuffer*bounds.low)  { return "⬇️"; }
                    return "🟢";
                  }(),
                  buffer,"/",desiredBuffer,
                  { 
                    keepAway: keepAway,
                    serverDelay: Math.round(controller.control.serverDelay.get()),
                    jitter: Math.round(controller.jitter.get())
                  },
                  "Decoding time:",Math.round(Math.max.apply(null,Object.values(main.api.decodeTime))),
                  "Decoding queues:",Math.max.apply(null,Object.values(main.api.decodeQueue)),
                  "Earliness:",Math.round(Math.min.apply(null,Object.values(main.api.earliness))),
                  //"Server jitter:",msg.jitter,
                  "Available:",msg.end - msg.current, 
                  "Tweak:",
                  function(t){
                    if (t > 1) { return "↗️"; }
                    if (t < 1) { return "↘️"; }
                    return "🟢";
                  }(controller.frameTiming.speed.tweak),
                  controller.frameTiming.speed.tweak
                ];
                if (MistVideo.info.type == "live") {
                  args.unshift("From live:",Math.round(msg.end*1e-2 - playbackPoint.timestamp*1e-5)/10,"s");
                }
                console.log.apply(null,args);
              }
              
              if ((buffer < desiredBuffer*bounds.low) && (msg.play_rate_curr != "fast-forward") && (controller.frameTiming.speed.tweak >= 1)) {

                var underflow = 0;
                if (buffer < 10) {
                  //if the buffer is empty, there may also be an underflow on the decoder output -> trackwriter timer
                  //aka, the decoded packets are shown in the video element too late
                  //add this lateness to the extra buffer we're requesting
                  var early = main.api.earliness;
                  if (early) underflow = -1*Math.min.apply(null,Object.values(early));
                  underflow = Math.max(0,underflow);
                }

                if (msg.current < msg.end) {
                  requestingMoreBuffer = true;
                  //request more data
                  control.send({
                    type: "fast_forward",
                    ff_add: desiredBuffer + underflow
                  });
                  if (controller.frameTiming.speed.tweak > 1) {
                    controller.frameTiming.tweakSpeed(1);
                  }
                  MistVideo.log("Our buffer ("+buffer+"ms) is small (<"+Math.round(desiredBuffer*bounds.low)+"ms), requesting more data (+"+Math.round(desiredBuffer + underflow)+"ms)..");
                  //test if we received enough data
                  var gotsetspeed = false;
                  control.addListener("set_speed").then(function(m){
                    gotsetspeed = true;
                    if (m.play_rate_prev == "fast-forward") {
                      control.addListener("on_time").then(function(m){
                        var newbuffer = 0;
                        var playbackPoint = controller.frameTiming.get("out");
                        var decodePoint = controller.frameTiming.get("in");
                        if (playbackPoint.timestamp && decodePoint.timestamp) { 
                          newbuffer = Math.round(decodePoint.timestamp*1e-3 - playbackPoint.timestamp*1e-3); //in ms
                        }
                        var increase = m.current - msg.current - (m._received - msg._received);
                        if (main.debugging) console.warn("Extra buffer received:",m.current - msg.current,"ms","Time taken:",m._received - msg._received,"ms","Increase:",increase,"ms");
                        if (buffer + increase < desiredBuffer*bounds.low) {
                          controller.frameTiming.tweakSpeed(tweaks.slower);
                          keepAway += 100;
                          MistVideo.log("Didn't receive enough extra data to increase our buffer ("+increase+"/"+Math.round(desiredBuffer*bounds.low - buffer)+"ms): slowing down..");
                          //once slowed down, the fast_forward request code will not trigger
                          //it may be tried again if the buffer shrinks again after playback speed returned to 1
                        }
                        else {
                          MistVideo.log("Received +"+increase+"ms extra data")
                        }
                        requestingMoreBuffer = false;
                      });
                    }
                    else {
                      //eh? reset
                      requestingMoreBuffer = false;
                    }
                  });
                  //it's possible we don't receive a set_speed answer - in that case there is no extra data available
                  control.addListener("on_time").then(function(m){
                    if (gotsetspeed) return;

                    if (requestingMoreBuffer && (m.play_rate_curr != "fast-forward")) {
                      requestingMoreBuffer = false;
                      controller.frameTiming.tweakSpeed(tweaks.slower);
                      keepAway += 100;
                      MistVideo.log("Didn't receive extra data: slowing down..");
                    }
                  });
                }
                else { //(msg.current >= msg.end)
                  if (controller.frameTiming.speed.main > 1) {
                    //if main playback speed is faster than real time, reset it to 1
                    //controller.frameTiming.setSpeed(1,tweaks.slower);
                    control.send({type:"set_speed",play_rate:"auto"});
                  }
                  controller.frameTiming.tweakSpeed(tweaks.slower);
                  MistVideo.log("Our buffer ("+buffer+"ms) is small (<"+Math.round(desiredBuffer*bounds.low)+"ms), but can't request more data: slowing down..");
                }
              }
              else {
                if ((controller.frameTiming.speed.tweak < 1) && (buffer >= desiredBuffer)) {
                  controller.frameTiming.tweakSpeed(1);
                  MistVideo.log("Our buffer ("+buffer+"ms) is large enough (>"+Math.round(desiredBuffer)+"ms), so return to normal playback.");
                }
                else {
                  if ((MistVideo.info.type == "live") && (MistVideo.options.liveCatchup)) { //in an else to prevent sending fast_forward more than once

                    //if the buffer is large, tweak playback speed to catch up
                    if ((msg.play_rate_curr == "auto") && controller.frameTiming) {
                      if ((controller.frameTiming.speed.tweak <= 1) && (buffer > desiredBuffer*bounds.high)) {
                        controller.frameTiming.tweakSpeed(tweaks.faster);
                        MistVideo.log("Our buffer ("+buffer+"ms) is big (>"+Math.round(desiredBuffer*bounds.high)+"ms), so tweak the playback speed to catch up.");

                      }
                      else if ((controller.frameTiming.speed.tweak > 1) && (buffer <= desiredBuffer)) {
                        controller.frameTiming.tweakSpeed(1);
                        MistVideo.log("Our buffer ("+buffer+"ms) is small enough (<"+Math.round(desiredBuffer)+"ms), so return to normal playback.");
                      }
                    }

                    //live catchup
                    if (msg.play_rate_curr != "fast-forward") {
                      var distanceToLive = msg.end - msg.current;
                      if (
                        (distanceToLive < MistVideo.options.liveCatchup*1e3)  // we're within a minute of the live point
                        && (distanceToLive > Math.max(msg.jitter*1.1,msg.jitter+250)) // the current (download) timestamp is more than jitter*1.1 and jitter+250 away from the live point
                        && (buffer-desiredBuffer < 1e3) // our buffer is less than a second larger than the desired buffer size
                      ) {
                        control.send({
                          type: "fast_forward",
                          ff_add: 5e3 //request an additional 5 seconds of data
                        });
                        MistVideo.log("We're away ("+(distanceToLive)+"ms) from the live point, requesting more data..");
                      }
                    }
                  }
                }
              }
            }
            

            controller.frameTiming.set("server",msg);
            //MistUtil.event.send("timeupdate",null,video);
          });
          controller.control.addListener("set_speed",function(msg){
            var speed;
            switch (msg.play_rate_curr) {
              case "auto":
              case "fast-forward": {
                //MistServer is controlling the playback rate
                //fast forwards means it is speeding now but it will return to auto once the requested position is reached (after seeking)
                speed = 1;
                break;
              }
              default: {
                speed = msg.play_rate_curr;
                break;
              }
            }
            controller.frameTiming.setSpeed(speed);
            controller.jitter.setSpeed(msg.play_rate_curr);
          });
          control.addSendListener("seek",function(msg){ //TODO multiple seeks at once can cause errors
            var seekTo = msg.seek_time; //[ms]
            MistUtil.event.send("seeking",seekTo*1e-3,video);
            controller.jitter.reset();

            if (main.debugging) console.warn("Seeking to ["+MistUtil.format.time(seekTo*1e-3)+"]: Emptying decoding and display queues");

            //flush frame queue and reset decoders
            for (var i in controller.pipelines) {
              controller.pipelines[i].empty();
              controller.pipelines[i].reset();
            }
            
            //set timestamp to target
            controller.frameTiming.seeking = seekTo*1e3; //[microseconds]

            controller.frameTiming.set("out",{
              baseTime: performance.now() - seekTo / main.controller.frameTiming.speed.combined, //[milliseconds]
              timestamp: seekTo*1e3 //[microseconds]
            });
            if (main.debugging) console.log("Frame timing was set to seek mode",{
              seeking: controller.frameTiming.seeking,
              out: controller.frameTiming.out
            });
            MistUtil.event.send("timeupdate",seekTo*1e-3,video); //[seconds]

          });
          control.addListener("pause",function(msg){
            if (msg.paused) controller.frameTiming.paused = true;
          });
          control.addSendListener("play",function(){
            //cancel frame timing - start playing at normal speed from the first queued packet
            controller.frameTiming.reset();
          });

          resolve();

        }).catch(function(err){
          MistVideo.showError(err);
          if (main.debugging) {
            console.error(err);
          }
        });
      });

      return this.connecting;
    }
    this.close = function(){
      this.control.removeListener("binary",controller.receiver);
      this.control.close();
      this.worker.terminate();
      if (controller.stream) {
        for (var i in controller.pipelines) {
          controller.pipelines[i].close();
        }
      }
    }

    var supportedCodecs = { audio: [], video: [] };
    function checkTracksAndConnect(){
      //check supported codecs if unknown, then connect
      var promises = [], thePlayer = mistplayers.rawws;
      if (!(MistVideo.stream in thePlayer.cacheSupported)) {
        thePlayer.cacheSupported[MistVideo.stream] = {}; 
      }
      var theCache = thePlayer.cacheSupported[MistVideo.stream];
      function checkTrack(i){
        function pushIfUnique(array,entry) {
          if (array.indexOf(entry) < 0) array.push(entry);
        }
        var track = MistVideo.info.meta.tracks[i];
        if ((track.type != "audio") && (track.type != "video")) { return; }
        if (i in theCache) {
          if (theCache[i]) {
            pushIfUnique(supportedCodecs[track.type],track.codec);
          }
        }
        else {
          promises.push(mistplayers.rawws.isTrackSupported(track).then(function(result){
            theCache[i] = result.supported;
            if (result.supported) {
              pushIfUnique(supportedCodecs[track.type],track.codec);
            }
          }));
        }
      }
      for (var i in MistVideo.info.meta.tracks) {
        checkTrack(i);
      }
      if (promises.length) {
        Promise.all(promises).then(connectIfEnough).catch(console.err);
      }
      else {
        connectIfEnough();
      }
    }
    function connectIfEnough() {
      var skip = false;
      //skip = true; //skip codec support step (for debugging)

      if (skip) {

        supportedCodecs = { audio: [], video: [] };
        for (var i in MistVideo.info.meta.tracks) {
          var track = MistVideo.info.meta.tracks[i];
          switch (track.type){
            case "audio":
            case "video": {
              if (supportedCodecs[track.type].indexOf(track.codec) < 0) supportedCodecs[track.type].push(track.codec);
            }
          }
        }

      }
      else {
        //calculate current player score
        var oldtracks = mistplayers.rawws.cacheSupported[MistVideo.stream].last;
        var myscore = MistVideo.scoreCombo(mistplayers.rawws.isBrowserSupported(MistVideo.source.type,MistVideo.source,MistVideo));
        if (myscore < 1) {
          //nothing (or only subtitles) can play, uhoh!
          MistVideo.log("After testing decoders, "+mistplayers[MistVideo.playerName].name+" cannot play any tracks after all: trying next combo..");
          MistVideo.nextCombo(false,"rawws cannot play any tracks");
          return;
        }

        var newtracks = mistplayers.rawws.cacheSupported[MistVideo.stream].last;
        for (var kind in oldtracks) {
          if (!(kind in newtracks)){
            //our score has decreased..

            //if not enough playable tracks, try to fall back to another player
            var firstCombo = MistVideo.checkCombo({startCombo:false},true);
            if (firstCombo.score > myscore && firstCombo.player != MistVideo.playerName) {
              MistVideo.log("After testing decoders, "+mistplayers[MistVideo.playerName].name+" is no longer the best scoring player, trying next combo..");
              MistVideo.nextCombo(false,"after testing, rawws is no longer the best player");
              return;
            }

            //our score is still best though
            break; 
          }
        }

        //this player is good - carry on
        MistVideo.log("Confirmed "+mistplayers[MistVideo.playerName].name+" is still the best scoring player, connecting..");
      }

      controller.connect();
    }

    checkTracksAndConnect();
  }
  this.controller = new RawPlayer(MistVideo.source.url);

  /////////////////////////////////////
  // WebCodec-specific api overrides //
  /////////////////////////////////////
  var custom_funcs = {};

  custom_funcs.currentTime = {
    get: function(){
      var frameTiming = main.controller.frameTiming;
      if (frameTiming) {
        var ts = frameTiming.get("out").timestamp; //contains the timestamp of the frame that has last passed to one of the track writers
        return ts ? ts*1e-6 : 0;
      }
      else return 0;
    },
    set: function(value){
      //send seek command
      //  MistServer will send new data:
      //  - starting with the first key frame before the specified seek_time
      //  - fast forward data to seek_time, plus additional ff_add
      main.controller.control.send({
        type: "seek",
        seek_time: (value == "live" ? "live" : Math.round(value*1e3)),
        ff_add: main.controller.desiredBuffer()
      });
    }
  };

  //the displayed buffer represents the frames that have been received from MistServer through the control channel, but have not yet been presented to the track writer
  //index 0 is What has been decoded but not yet passed to the track writer
  //index 1 is What was received but not yet decoded
  custom_funcs.buffered = {
    get: function(){
     return new function TimeRanges() {
       var frameTiming = main.controller.frameTiming;
       Object.defineProperty(this,"length",{
         get: function() { 
           if (frameTiming && frameTiming.get("in") && frameTiming.get("out")) return 2;
           return 0;
         }
       });
       this.start = function(index){
         if (frameTiming) {
           var info;
           switch (index) {
             case 0: info = frameTiming.get("out"); break;
             case 1: info = frameTiming.get("decoded"); break;
             default: throw new Error("Index out of bounds");
           }
           return info && ("timestamp" in info) ? info.timestamp*1e-6 : 0;
         }
       };
       this.end = function(index){
         if (frameTiming) {
           var info;
           switch (index) {
             case 0: info = frameTiming.get("decoded"); break;
             case 1: info = frameTiming.get("in"); break;
             default: throw new Error("Index out of bounds");
           }
           return info && ("timestamp" in info) ? info.timestamp*1e-6 : 0;
         }
       };
     }
    }
  };

  custom_funcs.duration = {
    get: function(){
      var frameTiming = main.controller.frameTiming;
      if (frameTiming) {
        var info = frameTiming.get("server");
        if (info && ("end" in info)) {
          if (MistVideo.info && (MistVideo.info.type == "live")) {
            return (info.end + new Date().getTime() - info._received.getTime())*1e-3;
          }
          else return info.end*1e-3;
        }
      }
      return 0;
    }
  };

  custom_funcs.stream_end = function(){
    //wait for buffers to complete, then send ended event
    var promises = [];
    for (var tid in main.controller.pipelines) {
      promises.push(main.controller.pipelines[tid].close(true));
    }
    Promise.all(promises).then(function(){
      MistUtil.event.send("ended",null,video);
    });
  };
  var looping = false;
  custom_funcs.restart = function(){
    if (looping) return;
    looping = true;
    //wait for buffers to complete, then loop
    var promises = [];
    for (var tid in main.controller.pipelines) {
      promises.push(main.controller.pipelines[tid].close(true));
    }
    Promise.all(promises).then(function(){
      MistVideo.log("Looping..");
      var result = main.controller.close();
      if (result instanceof Promise) {
        result.then(function(){
          //console.warn("closed, connecting");
          main.controller.connect().then(function(){
            looping = false;
            //console.warn("Looping complete");
          });
        });
      }
      else {
        main.controller.connect().then(function(){
          looping = false;
          //console.warn("Looping complete");
        });
      }
    });
  }


  /////////////////////////////
  // Expose additional stats //
  /////////////////////////////

  custom_funcs.receiveQueue = { //queue of chunks received over the control channel, but not yet passed to the decoder (because it is not yet configured) -> should be 0
    get: function(){
      var out = {};
      for (var i in main.controller.pipelines) {
        var pipeline = main.controller.pipelines[i];
        if (pipeline.track) {
          out[pipeline.track.type] = pipeline.queues.in.length;
        }
      }
      return out;
    }
  };
  custom_funcs.decodeQueue = { //browsers internal decoder queue -> should be 0
    get: function(){
      var out = {};
      for (var i in main.controller.pipelines) {
        var pipeline = main.controller.pipelines[i];
        if (pipeline.track) {
          out[pipeline.track.type] = pipeline.decoder.decodeQueueSize;
        }
      }
      return out;
    }
  };
  custom_funcs.displayQueue = { //queue of frames that have been decoded, but are not yet passed to the video element (track writer) -> should be > 0
    get: function(){
      var out = {};
      for (var i in main.controller.pipelines) {
        var pipeline = main.controller.pipelines[i];
        if (pipeline.track) {
          out[pipeline.track.type] = pipeline.queues.out.length;
        }
      }
      return out;
    }
  };
  custom_funcs.displayBuffer = { //duration of frames that have been decoded, but are not yet passed to the video element (track writer) -> should be desired buffer size
    get: function(){
      var out = {};
      for (var i in main.controller.pipelines) {
        var pipeline = main.controller.pipelines[i];
        if (pipeline.track) {
          if (pipeline.queues.out.length) {
            var first = pipeline.queues.out[0];
            var last = pipeline.queues.out[pipeline.queues.out.length-1];
            out[pipeline.track.type] = (last.timestamp - first.timestamp)*1e-3;
          }
          else {
            out[pipeline.track.type] = 0;
          }
        }
      }
      return out;
    }
  };
  custom_funcs.earliness = { 
    get: function(){
      var out = {};
      for (var i in main.controller.pipelines) {
        var pipeline = main.controller.pipelines[i];
        if (pipeline.track) {
          if ("early" in pipeline.stats) {
            out[pipeline.track.type] = pipeline.stats.early;
          }
          else {
            out[pipeline.track.type] = 0;
          }
        }
      }
      return out;
    }
  };
  custom_funcs.sync = { //difference in output timestamps between video and audio tracks, in ms
    get: function(){
      var audio;
      var video;

      for (var i in main.controller.pipelines) {
        var pipeline = main.controller.pipelines[i];
        if (pipeline.track) {
          if (pipeline.closeWhenEmpty) continue;
          switch (pipeline.track.type) {
            case "audio": {
              audio = pipeline; break;
            }
            case "video": {
              video = pipeline; break;
            }
          }
        }
      }
      function get(p) {
        if (p.queues.out.length) return p.queues.out[0].timestamp;
        throw 1;
      }
      try {
        return Math.abs(get(audio) - get(video))*1e-3;
      }
      catch {
        return 0;
      }
    }
  };

  custom_funcs.framerate_in = {
    get: function() {
      return function(){
        var out = {};
        for (var i in main.controller.pipelines) {
          var pipeline = main.controller.pipelines[i];
          if (pipeline.track) {
            if ("in" in pipeline.stats.framerate) {
              out[pipeline.track.type] = pipeline.stats.framerate.in.get();
            }
            else {
              out[pipeline.track.type] = 0;
            }
          }
        }
        return out;
      }
    }
  };
  custom_funcs.framerate_out = {
    get: function() {
      return function(){
        var out = {};
        for (var i in main.controller.pipelines) {
          var pipeline = main.controller.pipelines[i];
          if (pipeline.track) {
            if ("out" in pipeline.stats.framerate) {
              out[pipeline.track.type] = pipeline.stats.framerate.out.get();
            }
            else {
              out[pipeline.track.type] = 0;
            }
          }
        }
        return out;
      }
    }
  };
  custom_funcs.local_jitter = {
    get: function() {
      return main.controller.jitter.get();
    }
  };
  custom_funcs.decodeTime = { //averaged time it takes for frames to decode [ms]
    get: function(){ 
      var out = {};
      for (var i in main.controller.pipelines) {
        var pipeline = main.controller.pipelines[i];
        if (pipeline.track) {
          if (pipeline.stats.timing.decoder) {
            out[pipeline.track.type] = pipeline.stats.timing.decoder.delay; 
          }
          else {
            out[pipeline.track.type] = 0;
          }
        }
      }
      return out;
    }
  };
  custom_funcs.displayTime = { //averaged time it takes for frames to pass the track writer [ms]
    get: function(){ 
      var out = {};
      for (var i in main.controller.pipelines) {
        var pipeline = main.controller.pipelines[i];
        if (pipeline.track) {
          if (pipeline.stats.timing.writable) {
            out[pipeline.track.type] = pipeline.stats.timing.writable.delay; 
          }
          else {
            out[pipeline.track.type] = 0;
          }
        }
      }
      return out;
    }
  };
  custom_funcs.shift = { //timestamp difference after exiting the decoder
    get: function(){ 
      var out = {};
      for (var i in main.controller.pipelines) {
        var pipeline = main.controller.pipelines[i];
        if (pipeline.track) {
          if (pipeline.stats.timing.decoder) {
            out[pipeline.track.type] = pipeline.stats.timing.decoder.shift; 
          }
          else {
            out[pipeline.track.type] = 0;
          }
        }
      }
      return out;
    }
  };


  this.api = new MistUtil.shared.ControlChannelAPI(main.controller,MistVideo,video,custom_funcs);


  this.api.unload = function(){
    main.controller.close();
    //if (myTimeout) myTimeout.destroy();
  };

  callback(video);
}
