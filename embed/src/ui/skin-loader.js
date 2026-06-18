import { MistUtil } from '../core/util.js';
import { MistSkins } from './default-skin.js';
import { resolveTheme } from '../core/themes.js';
import './dev-skin.js';

export function MistSkin(MistVideo) {
  MistVideo.skin = this;
  
  this.applySkinOptions = function(skinOptions) {
    if ((typeof skinOptions == "string") && (skinOptions in MistSkins)) { skinOptions = MistUtil.object.extend({},MistSkins[skinOptions],true); }
    
    var skinParent;
    if (("inherit" in skinOptions) && (skinOptions.inherit) && (skinOptions.inherit in MistSkins)) {
      skinParent = this.applySkinOptions(skinOptions.inherit);
    }
    else {
      skinParent = MistSkins.default;
    }
    
    //structure should be shallow extended
    this.structure = MistUtil.object.extend({},skinParent.structure);
    if (skinOptions && ("structure" in skinOptions)) {
      MistUtil.object.extend(this.structure,skinOptions.structure);
    }
    
    //blueprints should be shallow extended
    this.blueprints = MistUtil.object.extend({},skinParent.blueprints);
    if (skinOptions && ("blueprints" in skinOptions)) {
      MistUtil.object.extend(this.blueprints,skinOptions.blueprints);
    }
    
    //icons should be shallow extended
    this.icons = MistUtil.object.extend({},skinParent.icons,true);
    if (skinOptions && ("icons" in skinOptions)) {
      MistUtil.object.extend(this.icons.blueprints,skinOptions.icons);
    }
    this.icons.build = function(type,size,options){
      if (!size) { size = 22; }
      
      //return an svg
      
      var d = this.blueprints[type];
      var svg;
      if (typeof d.svg == "function") {
        svg = d.svg.call(MistVideo,options);
      }
      else {
        svg = d.svg;
      }
      
      if (typeof size != "object") {
        size = {
          height: size,
          width: size
        };
      }
      
      if (typeof d.size != "object") {
        d.size = {
          height: d.size,
          width: d.size
        };
      }
      if ((!("width" in size) && ("height" in size)) || (!("height" in size) && ("width" in size))) {
        if ("width" in size) {
          size.height = size.width * d.size.height / d.size.width;
        }
        if ("height" in size) {
          size.width = size.height * d.size.width / d.size.height;
        }
      }
      
      var str = "";
      str += '<svg viewBox="0 0 '+d.size.width+' '+d.size.height+'"'+("width" in size ? ' width="'+size.width+'"' : "")+("height" in size ? ' height="'+size.height+'"' : "")+' class="mist icon '+type+'">';
      str += '<svg xmlns="http://www.w3.org/2000/svg" version="1.1" height="100%" width="100%">';
      str += svg;
      str += '</svg>';
      str += '</svg>';
      
      var container = document.createElement("div");
      container.innerHTML = str;
      
      return container.firstChild;
    }
    
    //colors should be deep extended
    this.colors = MistUtil.object.extend({},skinParent.colors);
    if (skinOptions && ("colors" in skinOptions)) {
      MistUtil.object.extend(this.colors,skinOptions.colors,true);
    }

    //tokens should be shallow extended (direct --mist-* custom property overrides)
    this.tokens = MistUtil.object.extend({},skinParent.tokens || {});
    if (skinOptions && ("tokens" in skinOptions)) {
      MistUtil.object.extend(this.tokens,skinOptions.tokens);
    }

    //apply "general" css and  skin specific css to structure
    this.css = MistUtil.object.extend({},skinParent.css);
    if (skinOptions && ("css" in skinOptions)) {
      MistUtil.object.extend(this.css,skinOptions.css);
    }
    
    return this;
  }
  this.applySkinOptions("skin" in MistVideo.options ? MistVideo.options.skin : "default");

  if (MistVideo.options.theme) {
    var themeTokens = resolveTheme(MistVideo.options.theme, MistVideo.options.themeMode);
    if (themeTokens) {
      MistUtil.object.extend(this.tokens, themeTokens);
    }
  }

  this.setToken = function(name, value) {
    if (MistVideo.container) {
      MistVideo.container.style.setProperty(
        name.indexOf('--') === 0 ? name : '--mist-' + name, value
      );
    }
  };
  this.setTokens = function(tokens) {
    for (var name in tokens) { this.setToken(name, tokens[name]); }
  };

  //load css
  var styles = [];
  for (var i in this.css) {
    if (typeof this.css[i] == "string") {
      var cssUrl = this.css[i];
      if (MistVideo.options.host && !/^https?:\/\//.test(cssUrl)) {
        var path = cssUrl.replace(/^(\.\.\/)+/,'');
        if (path[0] !== '/') path = '/' + path;
        cssUrl = MistVideo.options.host.replace(/\/$/,'') + path;
      }
      var a = MistUtil.css.load(MistVideo.urlappend(cssUrl),this.colors);
      styles.push(a);
    }
  }
  this.css = styles; //overwrite 
  
  return;
}
