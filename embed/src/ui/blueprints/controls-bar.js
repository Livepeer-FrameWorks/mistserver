import { MistUtil } from '../../core/util.js';

export const controlsBarBlueprints = {
    controls: function(){
      if ((this.options.controls) && (this.options.controls != "stock")) {
        MistUtil.class.add(this.container,"hasControls");
        
        var container = this.UI.buildStructure(this.skin.structure.controls);
        if (MistUtil.isTouchDevice() && this.size && (this.size.width > 300)) {
          container.style.zoom = 1.5;
        }
        return container;
      }
    },
    submenu: function(){
      return this.UI.buildStructure(this.skin.structure.submenu);
    },
    hoverWindow: function(options){
      
      //rewrite to a container with specific classes and continue the buildStructure call
      
      var structure = {
        type: "container",
        classes: ("classes" in options ? options.classes : []),
        children: ("children" in options ? options.children : [])
      };
      
      
      structure.classes.push("hover_window_container");
      if (!("classes" in options.window)) { options.window.classes = []; }
      options.window.classes.push("inner_window");
      options.window.classes.push("mistvideo-container");
      options.window = {
        type: "container",
        classes: ["outer_window"],
        children: [options.window]
      };
      
      if (!("classes" in options.button)) { options.button.classes = []; }
      options.button.classes.push("pointer");
      
      switch (options.mode) {
        case "left":
          structure.classes.push("horizontal");
          structure.children = [options.window,options.button];
          break;
        case "right":
          structure.classes.push("horizontal");
          structure.children = [options.button,options.window];
          break;
        case "top":
          structure.classes.push("vertical");
          structure.children = [options.button,options.window];
          break;
        case "bottom":
          structure.classes.push("vertical");
          structure.children = [options.window,options.button];
          break;
        case "pos":
          structure.children = [options.button,options.window];
          if (!("classes" in options.window)) { options.window.classes = []; }
          break;
        default:
          throw "Unsupported mode for structure type hoverWindow";
          break;
      }
      
      if ("transition" in options) {
        
        if (!("css" in structure)) { structure.css = []; }
        structure.css.push(
          ".hover_window_container:hover > .outer_window:not([data-hidecursor]) > .inner_window { "+options.transition.show+" }\n"+
          ".hover_window_container > .outer_window { "+options.transition.viewport+" }\n"+
          ".hover_window_container > .outer_window > .inner_window { "+options.transition.hide+" }"
        );
        
      }
      
      structure.classes.push(options.mode);
      
      return this.UI.buildStructure(structure);
    },
    draggable: function(options){
      var container = this.skin.blueprints.container(options);
      var MistVideo = this;
      
      var button = this.skin.icons.build("fullscreen",16);
      MistUtil.class.remove(button,"fullscreen");
      MistUtil.class.add(button,"draggable-icon");
      container.appendChild(button);
      button.style.alignSelf = "flex-end";
      button.style.position = "absolute";
      button.style.cursor = "move";
      
      var offset = {};
      var move = function(e){
        container.style.left = (e.clientX - offset.x)+"px";
        container.style.top = (e.clientY - offset.y)+"px";
      };
      var stop = function(e){ 
        window.removeEventListener("mousemove",move);
        window.removeEventListener("click",stop);
        
        MistUtil.event.addListener(button,"click",start);
      };
      var start = function(e){
        e.stopPropagation();
        
        button.removeEventListener("click",start);
        
        offset.x = MistVideo.container.getBoundingClientRect().left - (container.getBoundingClientRect().left - e.clientX);
        offset.y = MistVideo.container.getBoundingClientRect().top - (container.getBoundingClientRect().top - e.clientY);
        
        container.style.position = "absolute";
        container.style.right = "auto";
        container.style.bottom = "auto";
        MistVideo.container.appendChild(container);
        move(e);
        
        //container.style.resize = "both";
        
        MistUtil.event.addListener(window,"mousemove",move,container);
        MistUtil.event.addListener(window,"click",stop,container);
      };
      
      MistUtil.event.addListener(button,"click",start);
      
      return container;
    },
};
