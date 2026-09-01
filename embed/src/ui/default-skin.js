import { MistUtil } from '../core/util.js';
import { allBlueprints } from './blueprints/index.js';

export const MistSkins = {};


MistSkins["default"] = {
  structure: {
    main: {
      if: function(){
        return (!!this.info.hasVideo && (this.source.type.split("/")[1] != "audio"));
      },
      then: { //use this substructure when there is video
        type: "placeholder",
        classes: ["mistvideo"],
        children: [{type: "contextMenu"},{
          type: "hoverWindow",
          classes: ["mistvideo-maincontainer"],
          mode: "pos",
          style: {position: "relative"},
          transition: {
            hide: "left: 0; right: 0; bottom: calc(-1 * var(--mist-control-height) - 1px);",
            show: "bottom: 0;",
            viewport: "left:0; right: 0; top: -1000px; bottom: 0;"
          },
          button: {type: "videocontainer"},
          children: [{type: "idleScreen"},{type: "loading"},{type:"keyControls"},{type: "error"}],
          window: {type: "controls"}
        }]
      },
      else: { //use this subsctructure for audio only
        type: "container",
        classes: ["mistvideo"],
        style: {overflow: "visible"},
        children: [
          {
            type: "controls",
            classes: ["mistvideo-novideo"],
            style: {width: "480px"}
          },
          {type: "idleScreen"},
          {type: "loading"},
          {type: "keyControls"},
          {type: "error"},
          {
            if: function(){
              return (this.options.controls == "stock");
            },
            then: { //show the video element if its controls will be used
              type: "video",
              style: { position: "absolute" }
            },
            else: { //hide the video element
              type: "video",
              style: {
                position: "absolute",
                display: "none"
              }
            }
          }
        ],
      }
    },
    videocontainer: {
      type: "container",
      children: [
        {type: "videobackground", alwaysDisplay: false, delay: 5 },
        {type: "video"},
        {type: "subtitles"}
      ]
    },
    controls: {
      if: function(){
        return !!(this.player && this.player.api && this.player.api.play)
      },
      then: { //use this subsctructure for players that have an api with at least a play function available
        type: "container",
        classes: ["mistvideo-column"],
        children: [
        {
          type: "progress",
          classes: ["mistvideo-pointer"]
        },
        {
          type: "container",
          classes: ["mistvideo-main","mistvideo-padding","mistvideo-row","mistvideo-background"],
          children: [
            {
              type: "seekBackward",
              classes: ["mistvideo-pointer","mistvideo-seek-btn"]
            },
            {
              type: "play",
              classes: ["mistvideo-pointer"]
            },
            {
              type: "seekForward",
              classes: ["mistvideo-pointer","mistvideo-seek-btn"]
            },
            {type: "currentTime"},
            {
              if: function(){
                //show the total time if the player size is larger than 300px
                if (("size" in this) && (this.size.width > 300) || ((!this.info.hasVideo || (this.source.type.split("/")[1] == "audio")))) {
                  return true;
                }
                return false;
              },
              then: {type: "totalTime"}
            },
            {
              type: "container",
              classes: ["mistvideo-align-right"],
              children: [
                {
                  type: "container",
                  classes: ["mistvideo-volume_group"],
                  children: [
                    {
                      type: "speaker",
                      classes: ["mistvideo-pointer"]
                    },
                    {
                      type: "container",
                      classes: ["mistvideo-volume_container"],
                      children: [{
                        type: "volume",
                        mode: "horizontal",
                        size: {height: 22},
                        classes: ["mistvideo-pointer"]
                      }]
                    }
                  ]
                },
                {
                  if: function(){
                    //show the fullscreen and loop buttons here if the player size is larger than 300px
                    if (("size" in this) && (this.size.width > 300) || ((!this.info.hasVideo || (this.source.type.split("/")[1] == "audio")))) {
                      return true;
                    }
                    return false;
                  },
                  then: {
                    type: "container",
                    children: [{
                      type: "chromecast",
                      classes: ["mistvideo-pointer"]
                    },{
                      type: "airplay",
                      classes: ["mistvideo-pointer"]
                    },{
                      type: "loop",
                      classes: ["mistvideo-pointer"]
                    },
                    {
                      type: "fullscreen",
                      classes: ["mistvideo-pointer"]
                    },{
                      type: "picture-in-picture",
                      classes: ["mistvideo-pointer"]
                    }]
                  }
                },
                {type: "settings", classes: ["mistvideo-pointer"]},
                {type: "submenu"}
              ]}
            ]
          }
        ]
      },
      else: { //use this subsctructure for players that don't have an api with at least a play function available
        if: function() { return !!(this.player && this.player.api); },
        then: { //use this subsctructure if some sort of api does exist
          type: "hoverWindow",
          mode: "pos",
          transition: {
            hide: "right: -1000px; bottom: calc(var(--mist-control-height) + 2px);",
            show: "right: var(--mist-space-xs);",
            viewport: "right: 0; left: -1000px; bottom: 0; top: -1000px"
          },
          style: { right: "5px", left: "auto" },
          button: {
            type: "settings",
            classes: ["mistvideo-background","mistvideo-padding"],
          },
          window: { type: "submenu" }
        }
      }
    },
    submenu: {
      type: "container",
      style: {
        "maxWidth": "22em",
        "minWidth": "12em",
        "zIndex": 2
      },
      classes: ["mistvideo-padding","mistvideo-column","mistvideo-background"],
      children: [
        {type: "tracks"},
        {
          if: function(){
            //only show the fullscreen and loop buttons here if the player size is less than 200px
            if (("size" in this) && (this.size.width <= 300)) {
              return true;
            }
            return false;
          },
          then: {
            type: "container",
            classes: ["mistvideo-center"],
            children: [{
              type: "chromecast",
              classes: ["mistvideo-pointer"]
            },{
              type: "loop",
              classes: ["mistvideo-pointer"]
            },
            {
              type: "fullscreen",
              classes: ["mistvideo-pointer"]
            },
            {
              type: "picture-in-picture",
              classes: ["mistvideo-pointer"]
            }]
          }
        }
      ]
    },
    placeholder: {
      type: "container",
      classes: ["mistvideo","mistvideo-delay-display"],
      children: [
        {type: "idleScreen"},
        {type: "placeholder"},
        {type: "loading"},
        {type: "error"}
      ]
    },
    secondaryVideo: function(switchThese){
      return {
        type: "hoverWindow",
        classes: ["mistvideo"],
        mode: "pos",
        transition: {
          hide: "left: var(--mist-space-md); bottom: calc(-1 * var(--mist-control-height) + 2px);",
          show: "bottom: var(--mist-space-md);",
          viewport: "left: 0; right: 0; top: 0; bottom: 0"
        },
        button: {
          type: "container",
          children: [{type: "videocontainer"}]
        },
        window: {
          type: "switchVideo",
          classes: ["mistvideo-controls","mistvideo-padding","mistvideo-background","mistvideo-pointer"],
          containers: switchThese
        }
      };
    }
  },
  css: {
    skin: "/skins/default.css"
  },
  icons: {
    blueprints: {
      play: {
        size: 45,
        svg: '<path d="M6.26004984594 3.0550109625C5.27445051914 3.68940862462 4.67905105702 4.78142391497 4.67968264562 5.95354422781C4.67968264562 5.95354422781 4.70004942312 39.0717540916 4.70004942312 39.0717540916C4.70302341604 40.3033886636 5.36331656075 41.439734231 6.43188211452 42.0521884912C7.50044766829 42.6646427515 8.81469531629 42.6600161659 9.87892235656 42.0400537716C9.87892235656 42.0400537716 38.5612768409 25.4802882606 38.5612768409 25.4802882606C39.6181165777 24.8606067582 40.2663250096 23.7262617523 40.2636734301 22.5011460995C40.2610218505 21.2760304467 39.6079092743 20.1445019555 38.5483970356 19.5294009803C38.5483970356 19.5294009803 9.84567577375 2.9709566275 9.84567577375 2.9709566275C8.72898008118 2.32550764609 7.34527425735 2.35794451351 6.26004984594 3.0550109625C6.26004984594 3.0550109625 6.26004984594 3.0550109625 6.26004984594 3.0550109625" class="fill" />'
      },
      largeplay: {
        size: 45,
        svg: '<path d="M6.26004984594 3.0550109625C5.27445051914 3.68940862462 4.67905105702 4.78142391497 4.67968264562 5.95354422781C4.67968264562 5.95354422781 4.70004942312 39.0717540916 4.70004942312 39.0717540916C4.70302341604 40.3033886636 5.36331656075 41.439734231 6.43188211452 42.0521884912C7.50044766829 42.6646427515 8.81469531629 42.6600161659 9.87892235656 42.0400537716C9.87892235656 42.0400537716 38.5612768409 25.4802882606 38.5612768409 25.4802882606C39.6181165777 24.8606067582 40.2663250096 23.7262617523 40.2636734301 22.5011460995C40.2610218505 21.2760304467 39.6079092743 20.1445019555 38.5483970356 19.5294009803C38.5483970356 19.5294009803 9.84567577375 2.9709566275 9.84567577375 2.9709566275C8.72898008118 2.32550764609 7.34527425735 2.35794451351 6.26004984594 3.0550109625C6.26004984594 3.0550109625 6.26004984594 3.0550109625 6.26004984594 3.0550109625" class="stroke" />'
      },
      pause: {
        size: 45,
        svg: '<g><path d="m 7.5,38.531275 a 4.0011916,4.0011916 0 0 0 3.749999,3.96873 l 2.2812501,0 a 4.0011916,4.0011916 0 0 0 3.96875,-3.75003 l 0,-32.28123 a 4.0011916,4.0011916 0 0 0 -3.75,-3.96875 l -2.2812501,0 a 4.0011916,4.0011916 0 0 0 -3.968749,3.75 l 0,32.28128 z" class="fill" /><path d="m 27.5,38.531275 a 4.0011916,4.0011916 0 0 0 3.75,3.9687 l 2.28125,0 a 4.0011916,4.0011916 0 0 0 3.96875,-3.75 l 0,-32.28126 a 4.0011916,4.0011916 0 0 0 -3.75,-3.96875 l -2.28125,0 a 4.0011916,4.0011916 0 0 0 -3.96875,3.75 l 0,32.28131 z" class="fill" /></g>'
      },
      speaker: {
        size: 45,
        svg: '<path d="m 32.737813,5.2037363 c -1.832447,-1.10124 -4.200687,-0.8622 -5.771871,0.77112 0,0 -7.738819,8.0443797 -7.738819,8.0443797 0,0 -3.417976,0 -3.417976,0 -1.953668,0 -3.54696,1.65618 -3.54696,3.68694 0,0 0,9.58644 0,9.58644 0,2.03094 1.593292,3.68712 3.54696,3.68712 0,0 3.417976,0 3.417976,0 0,0 7.738819,8.04474 7.738819,8.04474 1.572104,1.63404 3.938942,1.8747 5.771871,0.77076 0,0 0,-34.5914997 0,-34.5914997 z" class="stroke semiFill toggle" />'
      },
      volume: {
        size: {width:100, height:45},
        svg: function(){
          var uid = MistUtil.createUnique();
          return '<defs><mask id="'+uid+'"><path d="m6.202 33.254 86.029-28.394c2.6348-0.86966 4.7433 0.77359 4.7433 3.3092v28.617c0 1.9819-1.6122 3.5773-3.6147 3.5773h-86.75c-4.3249 0-5.0634-5.5287-0.40598-7.1098" fill="#fff" /></mask></defs><rect mask="url(#'+uid+')" class="slider horizontal semiFill" width="100%" height="100%" /><path d="m6.202 33.254 86.029-28.394c2.6348-0.86966 4.7433 0.77359 4.7433 3.3092v28.617c0 1.9819-1.6122 3.5773-3.6147 3.5773h-86.75c-4.3249 0-5.0634-5.5287-0.40598-7.1098" class="stroke" /><rect x="0" y="0" width="100%" height="100%" fill="rgba(0,0,0,0.001)"/>'; //rectangle added because Edge won't trigger mouse events above transparent areas
        }
      },
      muted: {
        size: 45,
        svg: '<g class="stroke" stroke-linecap="round" vector-effect="none" stroke-width="2"><path d="m19.815 5.9747-7.7388 8.0444h-3.418c-1.9537 0-3.547 1.6562-3.547 3.6869v9.5864c0 2.0309 1.5933 3.6871 3.547 3.6871h3.418l7.7388 8.0447c1.5721 1.634 3.9389 1.8747 5.7719.77076v-34.591c-1.8324-1.1014-4.2007-.86258-5.7719.77074z"/><path d="m30.032 27.86 9.8517-9.8517"/><path d="m30.032 18.008 9.8517 9.8517"/></g>'
      },
      unmuted: {
        size: 45,
        svg: '<g class="stroke" stroke-linecap="round" vector-effect="none" stroke-width="2"><path d="m19.815 5.9747-7.7388 8.0444h-3.418c-1.9537 0-3.547 1.6562-3.547 3.6869v9.5864c0 2.0309 1.5933 3.6871 3.547 3.6871h3.418l7.7388 8.0447c1.5721 1.634 3.9389 1.8747 5.7719.77076v-34.591c-1.8324-1.1014-4.2007-.86258-5.7719.77074z"/><path d="m29.578 28.432c1.7601-1.2785 2.8014-3.3226 2.8008-5.498.000642-2.1754-1.0407-4.2196-2.8008-5.498"/><path d="m32.197 31.051c2.4404-1.9883 3.8564-4.9693 3.8555-8.1172-.000179-3.1475-1.4168-6.1278-3.8574-8.1152"/></g>'
      },
      fullscreen: {
        size: 45,
        svg: '<path d="m2.5 10.928v8.5898l4.9023-2.8008 9.6172 5.7832-9.6172 5.7832-4.9023-2.8008v8.5898h15.031l-4.9004-2.8008 9.8691-5.6387 9.8691 5.6387-4.9004 2.8008h15.031v-8.5898l-4.9023 2.8008-9.6172-5.7832 9.6172-5.7832 4.9023 2.8008v-8.5898h-15.033l4.9023 2.8008-9.8691 5.6387-9.8691-5.6387 4.9023-2.8008z" class="fill">'
      },
      pip: {
        size: 45,
        svg: '<rect x="5.25" y="12.25" width="34.5" height="19.5" ry="2" class="stroke semiFill toggle"/><rect x="20" y="21" width="17" height="8" rx="2" ry="2" class="semiFill toggle stroke"/>'
      },
      loop: {
        size: 45,
        svg: '<path d="M 21.279283,3.749797 A 18.750203,18.750203 0 0 0 8.0304417,9.2511582 L 12.740779,13.961496 A 12.083464,12.083464 0 0 1 21.279283,10.416536 12.083464,12.083464 0 0 1 33.362748,22.5 12.083464,12.083464 0 0 1 21.279283,34.583464 12.083464,12.083464 0 0 1 12.740779,31.038504 l 3.063185,-3.063185 H 4.9705135 V 38.80877 L 8.0304417,35.748842 A 18.750203,18.750203 0 0 0 21.279283,41.250203 18.750203,18.750203 0 0 0 40.029486,22.5 18.750203,18.750203 0 0 0 21.279283,3.749797 Z" class="stroke semiFill toggle" />'
      },
      settings: {
        size: 45,
        svg: '<path d="m24.139 3.834-1.4785 4.3223c-1.1018 0.0088-2.2727 0.13204-3.2031 0.33594l-2.3281-3.9473c-1.4974 0.45304-2.9327 1.091-4.2715 1.9004l1.3457 4.3672c-0.87808 0.62225-1.685 1.3403-2.4023 2.1426l-4.1953-1.8223c-0.9476 1.2456-1.7358 2.6055-2.3457 4.0469l3.6523 2.7383c-0.34895 1.0215-0.58154 2.0787-0.69336 3.1523l-4.4531 0.98828c-0.00716 0.14696-0.011931 0.29432-0.015625 0.44141 0.00628 1.4179 0.17336 2.8307 0.49805 4.2109l4.5703 0.070312c0.32171 1.0271 0.75826 2.0138 1.3008 2.9434l-3.0391 3.4355c0.89502 1.2828 1.9464 2.4492 3.1309 3.4707l3.7363-2.6289c0.86307 0.64582 1.7958 1.192 2.7812 1.6289l-0.43555 4.541c1.4754 0.52082 3.0099 0.85458 4.5684 0.99414l1.4766-4.3223c0.05369 3e-3 0.10838 0.005313 0.16211 0.007812 1.024-0.0061 2.0436-0.12048 3.043-0.34375l2.3281 3.9473c1.4974-0.45304 2.9327-1.091 4.2715-1.9004l-1.3457-4.3672c0.87808-0.62225 1.685-1.3403 2.4023-2.1426l4.1953 1.8223c0.9476-1.2456 1.7358-2.6055 2.3457-4.0469l-3.6523-2.7383c0.34895-1.0215 0.58154-2.0787 0.69336-3.1523l4.4531-0.98828c0.0072-0.14698 0.011925-0.29432 0.015625-0.44141-0.0062-1.4179-0.17336-2.8307-0.49805-4.2109l-4.5703-0.070312c-0.32171-1.0271-0.75826-2.0138-1.3008-2.9434l3.0391-3.4355c-0.89502-1.2828-1.9464-2.4492-3.1309-3.4707l-3.7363 2.6289c-0.86307-0.64582-1.7958-1.192-2.7812-1.6289l0.43555-4.541c-1.4754-0.52082-3.0099-0.85457-4.5684-0.99414zm-1.6387 7.8789a10.786 10.786 0 0 1 10.787 10.787 10.786 10.786 0 0 1-10.787 10.787 10.786 10.786 0 0 1-10.787-10.787 10.786 10.786 0 0 1 10.787-10.787z" class="fill"/>'
      },
      loading: {
        size: 100,
        svg: '<path d="m49.998 8.7797e-4c-0.060547 0.0018431-0.12109 0.0037961-0.18164 0.0058593-0.1251 0.0015881-0.25012 0.0061465-0.375 0.013672h-0.001954c-27.388 0.30599-49.432 22.59-49.439 49.98 0.020074 2.6488 0.25061 5.292 0.68945 7.904 3.8792-24.231 24.77-42.065 49.311-42.096v-0.0058582h0.001954c4.3638 3.0803e-4 7.9013-3.5366 7.9021-7.9002 1.474e-4 -2.0958-0.83235-4.106-2.3144-5.5879-1.482-1.482-3.492-2.3145-5.5879-2.3144-6.5007e-4 -7.9369e-8 -0.0013001-7.9369e-8 -0.001954 0" class="semiFill spin"></path>'
      },
      timeout: {
        size: 25,
        svg: function(options){
          if ((!options) || (!options.delay)) {
            options = {delay: 10};
          }
          var delay = options.delay;
          var uid = MistUtil.createUnique();
          return '<defs><mask id="'+uid+'"><rect x="0" y="0" width="25" height="25" fill="#fff"/><rect x="-5" y="-5" width="17.5" height="35" fill="#000" transform="rotate(180,12.5,12.5)"><animateTransform attributeName="transform" type="rotate" from="0,12.5,12.5" to="180,12.5,12.5" begin="DOMNodeInsertedIntoDocument" dur="'+(delay/2)+'s" repeatCount="1"/></rect><rect x="0" y="0" width="12.5" height="25" fill="#fff"/><rect x="-5" y="-5" width="17.5" height="35" fill="#000" transform="rotate(360,12.5,12.5)"><animate attributeType="CSS" attributeName="opacity" from="0" to="1" begin="DOMNodeInsertedIntoDocument" dur="'+(delay)+'s" calcMode="discrete" repeatCount="1" /><animateTransform attributeName="transform" type="rotate" from="180,12.5,12.5" to="360,12.5,12.5" begin="DOMNodeInsertedIntoDocument+'+(delay/2)+'s" dur="'+(delay/2)+'s" repeatCount="1"/></rect><circle cx="12.5" cy="12.5" r="8" fill="#000"/></mask></defs><circle cx="12.5" cy="12.5" r="12.5" class="fill" mask="url(#'+uid+')"/>';
        }
      },
      popout: {
        size: 45,
        svg: '<path d="m24.721 11.075c-12.96 0.049575-32.113 15.432-10.336 28.834-7.6763-7.9825-2.4795-21.824 10.336-22.19v5.5368l15.276-8.862-15.276-8.86v5.5419z" class="stroke fill"/>'
      },
      switchvideo: {
        size: 45,
        svg: '<path d="m8.4925 18.786c-3.9578 1.504-6.4432 3.632-6.4434 5.9982 2.183e-4 4.1354 7.5562 7.5509 17.399 8.1467v4.7777l10.718-6.2573-10.718-6.2529v4.5717c-6.9764-0.4712-12.229-2.5226-12.227-4.9859 6.693e-4 -0.72127 0.45868-1.4051 1.2714-2.0267zm28.015 0v3.9715c0.81164 0.62126 1.2685 1.3059 1.2692 2.0267-0.0014 1.4217-1.791 2.75-4.8021 3.6968-2.0515 0.82484-0.93693 3.7696 1.2249 2.9659 5.3088-1.8593 8.7426-3.8616 8.7514-6.6627-1.26e-4 -2.3662-2.4856-4.4942-6.4434-5.9982z" class="fill"/><rect rect x="10.166" y="7.7911" width="24.668" height="15.432" class="stroke"/>'
      },
      forward: {
        size: 45,
        svg: '<g class="stroke" style="fill:none;stroke-linecap:round;stroke-linejoin:round;stroke-width:6"><path d="m26.144 11.568 7.2881 10.932-7.2881 10.932"/><path d="m11.568 11.568 7.2881 10.932-7.2881 10.932"/></g>'
      },
      backward: {
        size: 45,
        svg: '<g class="stroke" style="fill:none;stroke-linecap:round;stroke-linejoin:round;stroke-width:6;transform:scaleX(-1);transform-origin:center"><path d="m26.144 11.568 7.2881 10.932-7.2881 10.932"/><path d="m11.568 11.568 7.2881 10.932-7.2881 10.932"/></g>'
      },
      right: {
        size: 45,
        svg: '<path class="fill"  d="m3.5 26.048c0 1.4295 1.1443 2.5803 2.5656 2.5803h21.975v7.4083l13.459-13.537-13.459-13.537v7.4083h-21.975c-1.4214 0-2.5656 1.1508-2.5656 2.5803z" style="stroke-linejoin:round;stroke-width:2">'
      },
      left: {
        size: 45,
        svg: '<path class="fill"  d="m3.5 26.048c0 1.4295 1.1443 2.5803 2.5656 2.5803h21.975v7.4083l13.459-13.537-13.459-13.537v7.4083h-21.975c-1.4214 0-2.5656 1.1508-2.5656 2.5803z" style="stroke-linejoin:round;stroke-width:2;transform:scaleX(-1);transform-origin:center">'
      },
      seekforward: {
        size: 45,
        svg: '<g class="stroke" style="fill:none;stroke-linecap:round;stroke-linejoin:round;stroke-width:3"><path d="M22.5 6.5A16 16 0 1 1 9.2 12.8"/><path d="M22.5 2v9h-9"/></g><text x="22.5" y="28" text-anchor="middle" class="fill" style="font-size:11px;font-family:sans-serif;stroke:none">10</text>'
      },
      seekbackward: {
        size: 45,
        svg: '<g class="stroke" style="fill:none;stroke-linecap:round;stroke-linejoin:round;stroke-width:3"><path d="M22.5 6.5A16 16 0 1 0 35.8 12.8"/><path d="M22.5 2v9h9"/></g><text x="22.5" y="28" text-anchor="middle" class="fill" style="font-size:11px;font-family:sans-serif;stroke:none">10</text>'
      },
      airplay: {
        size: 45,
        svg: '<g class="stroke" style="fill:none;stroke-linecap:round;stroke-linejoin:round;stroke-width:3"><path d="M5 36V10a3 3 0 0 1 3-3h29a3 3 0 0 1 3 3v26"/><polygon points="22.5,38 11,26 34,26" class="fill"/></g>'
      }
    }
  },
  blueprints: allBlueprints,
  colors: {
    fill: "#fff",
    semiFill: "rgba(255,255,255,0.5)",
    stroke: "#fff",
    strokeWidth: 1.5,
    background: "rgba(0,0,0,0.8)",
    progressBackground: "#333",
    accent: "#0f0"
  }
};

MistSkins["minimal"] = {
  inherit: "default",
  structure: {
    main: {
      if: function(){
        return (!!this.info.hasVideo && (this.source.type.split("/")[1] != "audio"));
      },
      then: {
        type: "container",
        classes: ["mistvideo"],
        children: [
          {type: "videocontainer"},
          {
            type: "container",
            classes: ["mistvideo-column"],
            children: [
              {type: "progress", classes: ["mistvideo-pointer"]},
              {
                type: "container",
                classes: ["mistvideo-main","mistvideo-padding","mistvideo-row","mistvideo-background"],
                children: [
                  {type: "play", classes: ["mistvideo-pointer"]},
                  {type: "currentTime"},
                  {
                    type: "container",
                    classes: ["mistvideo-align-right"],
                    children: [
                      {type: "fullscreen", classes: ["mistvideo-pointer"]}
                    ]
                  }
                ]
              }
            ]
          },
          {type: "idleScreen"},
          {type: "loading"},
          {type: "error"}
        ]
      },
      else: {
        type: "container",
        classes: ["mistvideo"],
        children: [
          {
            type: "container",
            classes: ["mistvideo-column"],
            children: [
              {type: "play", classes: ["mistvideo-pointer"]},
              {type: "progress", classes: ["mistvideo-pointer"]}
            ]
          },
          {type: "idleScreen"},
          {type: "loading"},
          {type: "error"}
        ]
      }
    },
    videocontainer: {
      type: "container",
      children: [
        {type: "video"},
        {type: "subtitles"}
      ]
    }
  },
  css: {
    skin: "/skins/default.css"
  }
};

MistSkins["branded"] = {
  inherit: "default",
  colors: {
    fill: "#e8e8e8",
    semiFill: "rgba(232,232,232,0.4)",
    stroke: "#e8e8e8",
    strokeWidth: 2,
    background: "rgba(15,15,25,0.92)",
    progressBackground: "#2a2a3a",
    accent: "#e63946"
  },
  tokens: {
    '--mist-radius-sm': '6px',
    '--mist-radius-md': '8px',
    '--mist-progress-height': '3px',
    '--mist-progress-height-hover': '8px',
    '--mist-progress-knob-size': '6px'
  },
  blueprints: {
    logo: function() {
      var container = document.createElement("div");
      container.className = "mistvideo-brand-logo";
      container.textContent = "BRAND";
      return container;
    }
  },
  css: {
    skin: "/skins/default.css"
  }
};

