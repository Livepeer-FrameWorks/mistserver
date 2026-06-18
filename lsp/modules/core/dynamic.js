export const dynamicUI = {
  dynamic: function(options,id){
    options = Object.assign({},options);

    const ele = options.create(id);
    if (!ele) { return; }
    ele.raw = "";
    if (!options.getEntry) {
      options.getEntry = function(d,id){
        return (id in d ? d[id] : false);
      };
    }
    if (!options.getEntries) {
      options.getEntries = function(d) {
        return d;
      };
    }
    if (ele.update) {
      options.update = ele.update;
    }
    if (!options.update) {
      options.update = function(){};
    }
    if (typeof options.add == "object") {
      const addoptions = options.add;
      options.add = function(id){
        return dynamicUI.dynamic(addoptions,id);
      };
    }

    if (options.add) {

      ele._children = {};
      ele.add = function(id) {
        const child = options.add.call(ele,id);
        if (!child) { return; }
        if (typeof child.remove != "function") {
          child.remove = function(){
            const child = this;
            if (child.parentNode) {
              child.parentNode.removeChild(child);
            }
            delete ele._children[id];
          };
        }
        else {
          const remove = child.remove;
          child.remove = function(){
            remove.apply(this,arguments);
            delete ele._children[id];
          };
        }

        child._id = id;
        ele._children[id] = child;
        if (child.customAdd) {
          child.customAdd(ele);
        }
        else {
          ele.append(child);
        }
      };
    }

    ele.update = function(values,allValues) {
      const entries = options.getEntries(values,ele._id);
      const raw = JSON.stringify(entries);
      if (this.raw == raw) {
        return;
      }
      this.values = entries;
      this.values_orig = values;


      if (options.add) {
        for (const id in entries) {
          const _existed = id in this._children;
          if (!_existed) {
            this.add(id);
          }
          if (id in this._children) {
            const _child = this._children[id];
            let _snap = null;
            if (_existed && _child.tagName === 'TR') {
              _snap = [];
              const _tds = _child.querySelectorAll('td');
              for (let _t = 0; _t < _tds.length; _t++) { _snap[_t] = _tds[_t].textContent; }
            }
            _child.update.call(_child,entries[id],entries);
            if (_snap) {
              const _tds = _child.querySelectorAll('td');
              for (let _t = 0; _t < _tds.length; _t++) {
                if (_snap[_t] && _snap[_t] !== _tds[_t].textContent) {
                  _tds[_t].classList.remove('cell-flash');
                  void _tds[_t].offsetWidth;
                  _tds[_t].classList.add('cell-flash');
                }
              }
            }
          }
        }
        for (const i in this._children) {
          if (!(i in entries)) {
            this._children[i].remove();
          }
        }
      }

      options.update.call(ele,entries,allValues);
      this.raw = raw;
    };

    if (options.values) ele.update.call(ele,options.values);

    return ele;
  }
};
