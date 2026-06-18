/**
 * Devices main tab - thumbnail grid or sortable table of discovered cameras.
 * Registered via ModeDispatch. Uses UI.dynamic, UI.context_menu, UI.pagecontrol.
 */
import { APP_NAME } from '@brand';
import { el, getval } from '../core/dom_helpers.js';
import { register } from '../core/mode_dispatch.js';
import { Component } from './component.js';
import { api } from './api.js';
import { dom } from './dom.js';
import { StreamDialog } from './stream_dialog.js';
import { uiHelpers } from '../core/ui_helpers.js';
import { apiClient } from '../core/api_client.js';
import { formEngine } from '../core/form_engine.js';
import { navto } from '../core/navigation.js';
import { dynamicUI } from '../core/dynamic.js';
import { uiCore } from '../core/ui_core.js';
import { mistHelpers } from '../core/mist_helpers.js';
import { sockets } from '../core/sockets.js';
import { defineSection } from '../core/section_registry.js';

defineSection({
  id: 'devices/list',
  page: 'Devices',
  title: 'Camera Devices',
  subtitle: 'Discovered cameras and device management',
  keywords: ['camera', 'device', 'ONVIF', 'VISCA', 'NDI', 'discovery', 'PTZ', 'snapshot', 'manufacturer', 'model', 'add camera'],
  requires: ['camera_list'],
  navTo: { tab: 'Devices', other: '' }
});

defineSection({
  id: 'devices/auto-streams',
  page: 'Devices',
  title: 'Auto-Add Camera Streams',
  subtitle: 'Automatically create streams for discovered cameras',
  keywords: ['auto add streams', 'camera streams', 'thumbnailing', 'MJPEG', 'auto camera'],
  requires: ['camera_list', 'capabilities'],
  navTo: { tab: 'Devices', other: '' }
});

function devicesPage(tab, other, prev, $main, $pageHeader) {
    if (!('camera_list' in mist.data) || !('capabilities' in mist.data) || !('streams' in mist.data)) {
      $main.innerHTML = 'Loading..';
      apiClient.send(function() {
        navto(tab, other);
      }, {camera_list: true, capabilities: true, streams: true});
      return;
    }

    const stored = mistHelpers.stored.get();
    let sortdevices = {by: 'name', dir: 1};
    let pagesize;
    if (other == '') {
      if ('devices_viewmode' in stored) {
        other = stored.devices_viewmode;
      } else {
        other = 'thumbnails';
      }
    }
    if ('sortdevices' in stored) {
      sortdevices = stored.sortdevices;
    }
    if ('devices_pagesize' in stored) {
      pagesize = stored.devices_pagesize;
    }

    let $devices;
    let $deviceView = el('span');
    $deviceView.show = function() { this.hidden = false; };
    $deviceView.hide = function() { this.hidden = true; };
    let $pagecontrol = el('span');
    $pagecontrol.show = function() { this.hidden = false; };
    $pagecontrol.hide = function() { this.hidden = true; };
    let current_devices = arrayToObject(mist.data.camera_list);

    const isThumbnails = (other === 'thumbnails');
    const viewToggle = el('button', {class: 'header-toggle view-toggle'});
    viewToggle.setAttribute('type', 'button');
    viewToggle.setAttribute('title', isThumbnails ? 'Thumbnail view' : 'List view');
    viewToggle.setAttribute('aria-label', isThumbnails ? 'Switch to list view' : 'Switch to thumbnail view');
    if (!isThumbnails) viewToggle.classList.add('toggled');

    const iconLeft = el('span', {class: 'toggle-icon toggle-icon-left toggle-icon-outside'});
    iconLeft.setAttribute('data-icon', 'grid');
    iconLeft.setAttribute('aria-hidden', 'true');

    const trackIconLeft = el('span', {class: 'toggle-icon toggle-icon-left toggle-icon-intrack'});
    trackIconLeft.setAttribute('data-icon', 'grid');
    trackIconLeft.setAttribute('aria-hidden', 'true');
    const trackThumb = el('span', {class: 'toggle-thumb'});
    const trackIconRight = el('span', {class: 'toggle-icon toggle-icon-right toggle-icon-intrack'});
    trackIconRight.setAttribute('data-icon', 'list');
    trackIconRight.setAttribute('aria-hidden', 'true');
    const track = el('span', {class: 'toggle-track'});
    track.setAttribute('aria-hidden', 'true');
    track.appendChild(trackIconLeft);
    track.appendChild(trackThumb);
    track.appendChild(trackIconRight);

    const iconRight = el('span', {class: 'toggle-icon toggle-icon-right toggle-icon-outside'});
    iconRight.setAttribute('data-icon', 'list');
    iconRight.setAttribute('aria-hidden', 'true');

    viewToggle.appendChild(iconLeft);
    viewToggle.appendChild(track);
    viewToggle.appendChild(iconRight);
    viewToggle.addEventListener('click', function() {
      const newMode = isThumbnails ? 'list' : 'thumbnails';
      mistHelpers.stored.set('devices_viewmode', newMode);
      navto('Devices', newMode);
    });

    const addCamBtn = el('button');
    addCamBtn.setAttribute('data-icon', 'plus');
    addCamBtn.textContent = 'Add camera';
    addCamBtn.addEventListener('click', function() {
      showAddCameraPopup();
    });

    const multiviewBtn = el('button');
    multiviewBtn.setAttribute('data-icon', 'grid');
    multiviewBtn.textContent = 'Multiview';
    multiviewBtn.addEventListener('click', function() {
      navto('Multiview');
    });

    uiHelpers.appendPageActions($pageHeader, [viewToggle, multiviewBtn, addCamBtn]);

    // --- Toggle toolbar (second row, inside page body) ---
    const toggleBar = el('div', {class: 'devices-toggle-bar'});

    // Discovery toggle (parent toggle for the loop itself; defaults to enabled)
    const discoveryToggle = el('button', {class: 'header-toggle discovery-toggle'});
    discoveryToggle.setAttribute('type', 'button');
    const discoveryEnabled = !mist.data.config || mist.data.config.device_discovery !== false;
    if (discoveryEnabled) discoveryToggle.classList.add('toggled');
    discoveryToggle.setAttribute('title', 'Continuously discover devices via ONVIF, VISCA and NDI');
    const discoveryTrack = el('span', {class: 'toggle-track'});
    discoveryTrack.setAttribute('aria-hidden', 'true');
    discoveryTrack.appendChild(el('span', {class: 'toggle-thumb'}));
    const discoveryLabel = el('span', {class: 'toggle-label'});
    discoveryLabel.textContent = 'Discovery';
    discoveryToggle.appendChild(discoveryTrack);
    discoveryToggle.appendChild(discoveryLabel);
    discoveryToggle.addEventListener('click', function() {
      const willEnable = !this.classList.contains('toggled');
      this.classList.toggle('toggled', willEnable);
      apiClient.send(function(d) {
        if (d && d.camera_config) {
          mist.data.config.device_discovery = d.camera_config.device_discovery;
        }
      }, {camera_config: {device_discovery: willEnable}});
    });
    toggleBar.appendChild(discoveryToggle);

    // Auto-add toggle
    const autoAddToggle = el('button', {class: 'header-toggle auto-add-toggle'});
    autoAddToggle.setAttribute('type', 'button');
    const autoAddEnabled = !!(mist.data.config && mist.data.config.auto_camera_streams);
    if (autoAddEnabled) autoAddToggle.classList.add('toggled');
    autoAddToggle.setAttribute('title', 'Auto-add all devices as streams');
    const autoAddTrack = el('span', {class: 'toggle-track'});
    autoAddTrack.setAttribute('aria-hidden', 'true');
    autoAddTrack.appendChild(el('span', {class: 'toggle-thumb'}));
    const autoAddLabel = el('span', {class: 'toggle-label'});
    autoAddLabel.textContent = 'Auto-add streams';
    autoAddToggle.appendChild(autoAddTrack);
    autoAddToggle.appendChild(autoAddLabel);
    autoAddToggle.addEventListener('click', function() {
      const willEnable = !this.classList.contains('toggled');
      if (!willEnable) {
        if (!confirm('Disable auto-add? This will remove all auto-created camera streams.')) return;
      }
      this.classList.toggle('toggled', willEnable);
      apiClient.send(function(d) {
        if (d && d.camera_config) {
          mist.data.config.auto_camera_streams = d.camera_config.auto_camera_streams;
          mist.data.config.auto_camera_thumbnailing = d.camera_config.auto_camera_thumbnailing;
        }
      }, {camera_config: {auto_camera_streams: willEnable}});
    });
    toggleBar.appendChild(autoAddToggle);

    // Thumbnailing toggle
    const caps = mist.data.capabilities && mist.data.capabilities.processes;
    const hasAV = caps && (caps.AV || caps.FFMPEG);
    const thumbProcess = (caps && caps.AV) ? 'AV' : 'FFMPEG';
    if (hasAV) {
      const thumbToggle = el('button', {class: 'header-toggle thumb-toggle'});
      thumbToggle.setAttribute('type', 'button');
      thumbToggle.setAttribute('title', 'Enable MJPEG thumbnailing on auto-camera streams');
      const thumbTrack = el('span', {class: 'toggle-track'});
      thumbTrack.setAttribute('aria-hidden', 'true');
      thumbTrack.appendChild(el('span', {class: 'toggle-thumb'}));
      const thumbLabel = el('span', {class: 'toggle-label'});
      thumbLabel.textContent = 'Thumbnailing';
      thumbToggle.appendChild(thumbTrack);
      thumbToggle.appendChild(thumbLabel);
      if (mist.data.config && mist.data.config.auto_camera_thumbnailing) thumbToggle.classList.add('toggled');
      thumbToggle.addEventListener('click', function() {
        const willEnable = !this.classList.contains('toggled');
        this.classList.toggle('toggled', willEnable);
        apiClient.send(function(d) {
          if (d && d.camera_config) {
            mist.data.config.auto_camera_streams = d.camera_config.auto_camera_streams;
            mist.data.config.auto_camera_thumbnailing = d.camera_config.auto_camera_thumbnailing;
          }
        }, {camera_config: {auto_camera_thumbnailing: willEnable}});
      });
      toggleBar.appendChild(thumbToggle);
    }

    const formEl = formEngine.buildUI([
      {
        type: 'help',
        classes: ['page-intro'],
        help: 'Discovered devices on your network. Devices are automatically found via ONVIF, VISCA, and NDI protocols. Click a device for details, or use the PTZ controls.'
      },
      {
        label: 'Filter devices',
        classes: ['filter'],
        help: 'Devices that do not match the text you enter here will be hidden.',
        'function': function() {
          const val = getval(this);
          if ($devices) $devices.filter(val);
        }
      }
    ]);
    formEl.classList.add('devices-filter-shell');

    let statusFilter = 'all';
    const statusFilterSpan = el('span', {class: 'devices-status-filter'});
    ['All', 'Online', 'Offline'].forEach(function(label) {
      const val = label.toLowerCase();
      const btn = el('button');
      btn.textContent = label;
      btn.addEventListener('click', function() {
        statusFilter = val;
        const allBtns = statusFilterSpan.querySelectorAll('button');
        Array.from(allBtns).forEach(function(b) { b.classList.remove('active'); });
        this.classList.add('active');
        applyStatusFilter();
      });
      if (val === 'all') btn.classList.add('active');
      statusFilterSpan.appendChild(btn);
    });
    const filterField = formEl.querySelector('.field.filter');
    if (filterField && filterField.parentNode) {
      filterField.parentNode.insertBefore(statusFilterSpan, filterField.nextSibling);
    }

    function isDeviceOnline(d) {
      return d && (d.status === 'online' || d.status === 'connected');
    }

    function applyStatusFilter() {
      if (!$devices) return;
      for (let i = 0; i < $devices.children.length; i++) {
        const item = $devices.children[i];
        if (statusFilter === 'all') {
          item.classList.remove('status-hidden');
          continue;
        }
        const id = item.getAttribute('data-id');
        const d = current_devices[id];
        const online = isDeviceOnline(d);
        if ((statusFilter === 'online' && online) || (statusFilter === 'offline' && !online)) {
          item.classList.remove('status-hidden');
        } else {
          item.classList.add('status-hidden');
        }
      }
      if ($devices.show_page) $devices.show_page();
    }

    const emptyIcon = el('div');
    emptyIcon.setAttribute('data-icon', 'monitor');
    const emptyH3 = el('h3');
    emptyH3.textContent = 'No devices discovered';
    const emptyP = el('p');
    emptyP.textContent = 'Devices will appear automatically as they are found via ONVIF, VISCA, and NDI on your network.';
    const emptyBtn = el('button');
    emptyBtn.setAttribute('data-icon', 'plus');
    emptyBtn.textContent = 'Add a camera';
    emptyBtn.addEventListener('click', function() { showAddCameraPopup(); });
    const emptyState = el('div', {class: 'streams-empty-state'});
    emptyState.appendChild(emptyIcon);
    emptyState.appendChild(emptyH3);
    emptyState.appendChild(emptyP);
    emptyState.appendChild(emptyBtn);

    function checkEmpty() {
      let hasDevices = false;
      for (const k in current_devices) { hasDevices = true; break; }
      if (hasDevices) {
        emptyState.hidden = true;
        formEl.hidden = false;
        $deviceView.show();
        $pagecontrol.show();
      } else {
        emptyState.hidden = false;
        formEl.hidden = true;
        $deviceView.hide();
        $pagecontrol.hide();
      }
    }

    $main.classList.add('page-body--flex-col', 'devices-page-body', 'devices-shell', 'slab-shell');
    const shell = $main;
    shell.appendChild(toggleBar);
    shell.appendChild(formEl);
    shell.appendChild(emptyState);

    const context_menu = new uiCore.context_menu();

    if (isThumbnails) {
      $devices = dynamicUI.dynamic({
        create: function() {
          const cont = document.createElement('div');
          cont.className = 'devices thumbnails';
          uiCore.sortableItems(cont, function(sortby) {
            return this.sortValues[sortby];
          }, {});
          return cont;
        },
        values: current_devices,
        add: {
          create: function(id) {
            const card = document.createElement('div');
            card.className = 'device';
            card.setAttribute('data-id', id);

            card.elements = {};

            card.elements.header = document.createElement('a');
            card.elements.header.className = 'header';
            card.appendChild(card.elements.header);

            card.elements.thumbnail = document.createElement('div');
            card.elements.thumbnail.className = 'thumbnail';
            card.appendChild(card.elements.thumbnail);

            card.elements.status = document.createElement('div');
            card.elements.status.className = 'device-status';
            card.appendChild(card.elements.status);

            card.elements.actions = document.createElement('div');
            card.elements.actions.className = 'actions';
            card.appendChild(card.elements.actions);

            card.elements.header.addEventListener('click', function() {
              navto('Camera Detail', id);
            });
            card.elements.thumbnail.addEventListener('click', function() {
              navto('Camera Detail', id);
            });
            const actionsBtn = document.createElement('button');
            actionsBtn.textContent = 'Actions';
            actionsBtn.addEventListener('click', function(e) {
              const rect = this.getBoundingClientRect();
              context_menu.fill(id, {pageX: rect.left + window.scrollX, pageY: rect.top + window.scrollY});
              e.stopPropagation();
            });
            card.elements.actions.appendChild(actionsBtn);

            card.addEventListener('contextmenu', function(e) {
              e.preventDefault();
              context_menu.fill(id, e);
            });

            card.remove = function() {
              if (this.parentNode) this.parentNode.removeChild(this);
            };

            return card;
          },
          update: function(data) {
            const header = this.elements.header;
            if (header._raw_name !== (data.name || data.id)) {
              header._raw_name = data.name || data.id;
              header.textContent = '';
              const protos = data.protocols || [];
              for (let pi = 0; pi < protos.length; pi++) {
                const pType = (protos[pi].type || protos[pi].protocol || '').toUpperCase();
                if (pType) {
                  const iconSpan = document.createElement('span');
                  iconSpan.className = 'cam-pill cam-proto-pill';
                  iconSpan.textContent = pType;
                  header.appendChild(iconSpan);
                }
              }
              header.appendChild(document.createTextNode(data.name || data.id || '-'));
            }

            var thumb = this.elements.thumbnail;
            var streamName = null;
            if (mist.data.streams) {
              if (data.streams) {
                var defIdx = data.defaultStream != null && data.defaultStream >= 0 ? data.defaultStream : 0;
                if (defIdx >= data.streams.length) defIdx = 0;
                var defUri = data.streams[defIdx] && data.streams[defIdx].uri;
                if (defUri) {
                  for (var sn in mist.data.streams) {
                    if (mist.data.streams[sn].source === defUri) { streamName = sn; break; }
                  }
                }
                if (!streamName) {
                  for (var si = 0; si < data.streams.length; si++) {
                    var stUri = data.streams[si].uri;
                    if (stUri) {
                      for (var sn2 in mist.data.streams) {
                        if (mist.data.streams[sn2].source === stUri) { streamName = sn2; break; }
                      }
                    }
                    if (streamName) break;
                  }
                }
              }
              if (!streamName) {
                var sanitized = (data.id || this._id).replace(/[^a-z0-9_\-\.]/gi, '_').toLowerCase();
                var conventionName = 'cam_' + sanitized;
                if (mist.data.streams[conventionName]) { streamName = conventionName; }
              }
            }

            if (isDeviceOnline(data) && streamName && thumb._mjpegActive !== streamName) {
              thumb._mjpegActive = streamName;
              var img = thumb.querySelector('img');
              if (!img) {
                img = document.createElement('img');
                img.alt = 'Camera preview';
                thumb.appendChild(img);
              }
              if (data.snapshotUri) img.src = data.snapshotUri;
              (function(imgRef) {
                sockets.ws.info_json.subscribe(function(wsData) {
                  if (!wsData.source) return;
                  var mjpegUrl = null;
                  var jpgUrl = null;
                  for (var j in wsData.source) {
                    if (wsData.source[j].type === 'html5/image/jpeg') {
                      var url = wsData.source[j].url;
                      if (url.indexOf('.mjpg') > -1) { mjpegUrl = url; }
                      else if (!jpgUrl) { jpgUrl = url; }
                    }
                  }
                  var displayUrl = mjpegUrl || jpgUrl;
                  if (displayUrl && imgRef.parentNode) { imgRef.src = displayUrl; }
                }, streamName);
              })(img);
            } else if (isDeviceOnline(data) && data.snapshotUri && !thumb._mjpegActive) {
              var img = thumb.querySelector('img');
              if (!img) {
                img = document.createElement('img');
                img.alt = 'Camera snapshot';
                thumb.appendChild(img);
              }
              var newSrc = data.snapshotUri + (data.snapshotUri.indexOf('?') >= 0 ? '&' : '?') + '_t=' + Date.now();
              if (img.src !== newSrc) img.src = newSrc;
            } else if (!isDeviceOnline(data)) {
              var existingImg = thumb.querySelector('img');
              if (existingImg) thumb.removeChild(existingImg);
              thumb._mjpegActive = null;
            }

            const statusDiv = this.elements.status;
            statusDiv.textContent = '';
            statusDiv.appendChild(dom.statusBadge(data.status));
            if (data.hasPTZ) statusDiv.appendChild(dom.pill('PTZ', 'green'));
            if (data.hasAudio) statusDiv.appendChild(dom.pill('Audio', 'green'));
            if (data.hasMetadata) statusDiv.appendChild(dom.pill('Analytics', 'green'));

            this.sortValues = {
              name: (data.name || data.id || '').toLowerCase(),
              status: isDeviceOnline(data) ? 1 : 0,
              host: (data.host || '').toLowerCase(),
              manufacturer: ((data.manufacturer || '') + ' ' + (data.model || '')).toLowerCase()
            };
          }
        },
        update: function() {
          this.sort();
          if (this.show_page) this.show_page();
          checkEmpty();
        }
      });
      $deviceView = $devices;
      $deviceView.show = function() { this.hidden = false; };
      $deviceView.hide = function() { this.hidden = true; };
      shell.appendChild($deviceView);

      let sort_index = sortdevices.by;
      const sort_dir = {name: 1, status: -1, host: 1, manufacturer: 1};
      let sort_reverse = (sort_dir[sort_index] || 1) * sortdevices.dir;

      const sortUI = formEngine.buildUI([{
        label: 'Sort devices by',
        help: 'Choose by which attribute the devices listed below should be sorted.',
        type: 'select',
        select: [
          ['name', 'Device name'],
          ['status', 'Status (Online / Offline)'],
          ['host', 'Host address'],
          ['manufacturer', 'Manufacturer / Model']
        ],
        value: sortdevices.by,
        'function': function() {
          sort_index = getval(this);
          sortdevices.by = sort_index;
          mistHelpers.stored.set('sortdevices', sortdevices);
          if ($devices) $devices.sort(sort_index, (sort_dir[sort_index] || 1) * sort_reverse);
        },
        'unit': (function() {
          const unitLabel = el('label');
          const checkbox = el('input');
          checkbox.setAttribute('type', 'checkbox');
          checkbox.checked = (sort_reverse === -1);
          checkbox.addEventListener('change', function() {
            sort_reverse = this.checked ? -1 : 1;
            sortdevices.dir = sort_reverse * (sort_dir[sort_index] || 1);
            mistHelpers.stored.set('sortdevices', sortdevices);
            if ($devices) $devices.sort(sort_index, (sort_dir[sort_index] || 1) * sort_reverse);
          });
          unitLabel.appendChild(checkbox);
          const reverseSpan = el('span');
          reverseSpan.textContent = 'Reverse';
          unitLabel.appendChild(reverseSpan);
          return unitLabel;
        })()
      }]);
      const sortChildren = Array.from(sortUI.children);
      for (let si = 0; si < sortChildren.length; si++) {
        formEl.appendChild(sortChildren[si]);
      }

    } else {
      // List view
      const tableEl = el('table', {class: 'devices table-wide'});
      $deviceView = el('div', {class: 'table-scroll devices-table-shell'});
      $deviceView.show = function() { this.hidden = false; };
      $deviceView.hide = function() { this.hidden = true; };
      $deviceView.appendChild(tableEl);
      shell.appendChild($deviceView);

      tableEl.layout = {
        status: function(d) {
          if (this._raw === d.status) return;
          this._raw = d.status;
          this.innerHTML = '';
          this.appendChild(dom.statusBadge(d.status));
        },
        name: function(d, id) {
          if (this._raw === id) return;
          this._raw = id;
          this.innerHTML = '';
          const rowTitle = el('div', {class: 'device-row-title'});
          const link = el('a', {class: 'clickable'});
          link.textContent = d.name || d.id || '-';
          link.addEventListener('click', function() {
            navto('Camera Detail', id);
          });
          rowTitle.appendChild(link);

          const actionBtn = el('button', {class: 'device-row-actions', type: 'button', title: 'Actions'});
          actionBtn.textContent = 'Actions';
          actionBtn.addEventListener('click', function(e) {
            const rect = this.getBoundingClientRect();
            context_menu.fill(id, {pageX: rect.left + window.scrollX, pageY: rect.top + window.scrollY});
            e.stopPropagation();
          });
          rowTitle.appendChild(actionBtn);

          this.appendChild(rowTitle);
        },
        protocol: function(d) {
          if (this._raw_p === d.protocols) return;
          this._raw_p = d.protocols;
          this.innerHTML = '';
          this.appendChild(dom.protocolPills(d.protocols));
        },
        host: function(d) {
          if (this._raw === d.host) return;
          this._raw = d.host;
          this.textContent = d.host || '-';
        },
        manufacturer: function(d) {
          const val = [d.manufacturer, d.model].filter(Boolean).join(' / ') || '-';
          if (this._raw === val) return;
          this._raw = val;
          this.textContent = val;
        },
        capabilities: function(d) {
          const key = (d.hasPTZ ? 'P' : '') + (d.hasAudio ? 'A' : '') + (d.hasMetadata ? 'M' : '');
          if (this._raw === key) return;
          this._raw = key;
          this.innerHTML = '';
          this.appendChild(dom.capabilityPills(d));
        }
      };

      const trHead = el('tr');
      trHead.setAttribute('data-sortby', 'name');
      const theadEl = el('thead', {class: 'sticky'});
      theadEl.appendChild(trHead);
      tableEl.appendChild(theadEl);
      const headers = {
        status: 'Status',
        name: 'Name',
        protocol: 'Protocols',
        host: 'Host',
        manufacturer: 'Manufacturer / Model',
        capabilities: 'Capabilities'
      };
      for (const col in tableEl.layout) {
        const label = headers[col] !== undefined ? headers[col] : col;
        const thEl = el('th');
        thEl.textContent = label;
        if (label !== '') thEl.setAttribute('data-index', col);
        trHead.appendChild(thEl);
      }

      $devices = dynamicUI.dynamic({
        create: function() {
          const tbody = document.createElement('tbody');
          uiCore.sortableItems(tbody, function(sortby) {
            const cell = this._cells[sortby];
            return cell ? cell._raw : null;
          }, {controls: trHead, sortsave: 'sortdevices'});
          tbody.remove = function() {
            if (this.parentNode) this.parentNode.removeChild(this);
          };
          return tbody;
        },
        values: current_devices,
        add: {
          create: function(id) {
            const row = document.createElement('tr');
            row.setAttribute('data-id', id);
            row._cells = {};
            for (const col in tableEl.layout) {
              const td = document.createElement('td');
              td.setAttribute('data-index', col);
              row._cells[col] = td;
              row.appendChild(td);
            }
            row.addEventListener('contextmenu', function(e) {
              e.preventDefault();
              context_menu.fill(id, e);
            });
            row.addEventListener('click', function() {
              navto('Camera Detail', id);
            });
            row.style.cursor = 'pointer';
            row.remove = function() {
              if (this.parentNode) this.parentNode.removeChild(this);
            };
            return row;
          },
          update: function(data) {
            for (const col in tableEl.layout) {
              tableEl.layout[col].call(this._cells[col], data, this._id);
            }
          }
        },
        update: function() {
          this.sort();
          if (this.show_page) this.show_page();
          checkEmpty();
        }
      });
      tableEl.appendChild($devices);
    }

    // Filter function
    $devices.filter = function(str) {
      str = (str || '').toLowerCase();
      for (let i = 0; i < this.children.length; i++) {
        const item = this.children[i];
        const id = item.getAttribute('data-id');
        const d = current_devices[id];
        if (!str) {
          item.classList.remove('hidden');
          continue;
        }
        const searchable = [d.name, d.id, d.host, d.manufacturer, d.model].filter(Boolean).join(' ').toLowerCase();
        if (searchable.indexOf(str) >= 0) {
          item.classList.remove('hidden');
        } else {
          item.classList.add('hidden');
        }
      }
      if ($devices.show_page) $devices.show_page();
    };

    // Context menu
    shell.appendChild(context_menu.ele);
    context_menu.fill = function(id, e) {
      const cam = current_devices[id];
      if (!cam) return;

      const headerEl = el('div', {class: 'header'});
      if (cam.protocols) {
        for (let pi = 0; pi < cam.protocols.length; pi++) {
          const pType = (cam.protocols[pi].type || cam.protocols[pi].protocol || '').toUpperCase();
          if (pType) {
            const pillSpan = el('span', {class: 'cam-pill cam-context-proto'});
            pillSpan.textContent = pType;
            headerEl.appendChild(pillSpan);
          }
        }
      }
      headerEl.appendChild(document.createTextNode(cam.name || cam.id || '-'));
      const statusText = cam.status || 'unknown';
      const statusBadge = el('span', {class: 'context-menu-badge'});
      statusBadge.classList.add('cam-context-status');
      statusBadge.classList.add(isDeviceOnline(cam) ? 'online' : 'offline');
      statusBadge.textContent = statusText.charAt(0).toUpperCase() + statusText.slice(1);
      headerEl.appendChild(statusBadge);

      const header = [headerEl];

      const gototabs = [
        ['Camera details', function() { navto('Camera Detail', id); }, 'info', 'View device info, streams, and configuration.']
      ];
      if (cam.hasPTZ) {
        gototabs.push(['PTZ Control', function() { navto('PTZ Control', id); }, 'move', 'Control camera pan, tilt, and zoom.']);
      }

      const streamActions = [];
      if (cam.streams && cam.streams.length) {
        streamActions.push(['Add stream to '+APP_NAME, function() {
          StreamDialog.show(id, cam.name, cam.streams[0], 0);
        }, 'plus', 'Create a '+APP_NAME+' stream from this camera.']);
      }

      const actions = [
        ['Remove device', function() {
          if (confirm('Remove this camera from the list?')) {
            api.removeCamera(id).then(function() {
              delete current_devices[id];
              $devices.update(current_devices);
            })['catch'](function(err) {
              alert('Failed to remove camera: ' + err.message);
            });
          }
        }, 'trash', 'Remove this device from the discovered list.']
      ];

      const menu = [header, gototabs];
      if (streamActions.length) menu.push(streamActions);
      menu.push(actions);

      if (cam.snapshotUri && isDeviceOnline(cam)) {
        const aside = el('aside');
        const previewImg = el('img', {class: 'cam-snapshot'});
        previewImg.setAttribute('src', cam.snapshotUri);
        previewImg.setAttribute('alt', 'Preview');
        aside.appendChild(previewImg);
        menu.push(aside);
      }

      context_menu.show(menu, e);
    };

    // Pagination
    const pagecontrolResult = uiCore.pagecontrol($devices, pagesize);
    $pagecontrol = pagecontrolResult;
    $pagecontrol.classList.add('devices-pagecontrol');
    $pagecontrol.show = function() { this.hidden = false; };
    $pagecontrol.hide = function() { this.hidden = true; };
    shell.appendChild($pagecontrol);
    $pagecontrol.elements.pagelength.addEventListener('change', function() {
      mistHelpers.stored.set('devices_pagesize', this.value);
    });

    // Auto-refresh
    var lastStreamKeys = Object.keys(mist.data.streams || {}).sort().join(',');
    UI.interval.set(function() {
      apiClient.send(function(d) {
        if (d && 'camera_list' in d) {
          mist.data.camera_list = d.camera_list;
          current_devices = arrayToObject(d.camera_list);

          var curStreamKeys = Object.keys(mist.data.streams || {}).sort().join(',');
          if (curStreamKeys !== lastStreamKeys) {
            $devices.raw = '';
            lastStreamKeys = curStreamKeys;
          }

          $devices.update(current_devices);
          applyStatusFilter();
        }
      }, {camera_list: true, streams: true});
    }, 10e3);

    checkEmpty();
}


function arrayToObject(arr) {
    const obj = {};
    if (!arr) return obj;
    for (let i = 0; i < arr.length; i++) {
      if (arr[i] && arr[i].id) {
        obj[arr[i].id] = arr[i];
      }
    }
    return obj;
}

function showAddCameraPopup() {
    const addData = {host: '', port: 80, protocol: 'onvif', username: '', password: '', name: ''};
    const modal = uiHelpers.openFormModal({
      size: 'md',
      title: 'Add Camera',
      form: [
      {label: 'Host', type: 'str', help: 'IP address or hostname of the camera.', pointer: {main: addData, index: 'host'}, validate: ['required']},
      {label: 'Port', type: 'int', help: 'Service port (default 80).', pointer: {main: addData, index: 'port'}, min: 1, max: 65535},
      {label: 'Protocol', type: 'select', select: [['onvif', 'ONVIF'], ['visca', 'VISCA']], help: 'Discovery protocol.', pointer: {main: addData, index: 'protocol'}},
      {label: 'Username', type: 'str', pointer: {main: addData, index: 'username'}},
      {label: 'Password', type: 'password', pointer: {main: addData, index: 'password'}},
      {label: 'Name', type: 'str', help: 'Optional display name.', pointer: {main: addData, index: 'name'}}
      ],
      buttons: [
        {label: 'Add', type: 'save', 'function': function() {
          const req = {host: addData.host, port: addData.port, protocol: addData.protocol};
          if (addData.name) req.name = addData.name;
          if (addData.username) req.username = addData.username;
          if (addData.password) req.password = addData.password;
          api.updateCamera(req).then(function() {
            modal.close();
            navto('Devices');
          })['catch'](function(err) {
            alert('Failed to add camera: ' + err.message);
          });
        }},
        {label: 'Cancel', type: 'cancel'}
      ]
    });
  }

register('Devices', {
  guided: devicesPage,
  advanced: devicesPage
});
