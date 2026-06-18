import { APP_NAME } from '@brand';
import { registerTab } from '../core/tab_registry.js';
import { formEngine } from '../core/form_engine.js';
import { el } from '../core/dom_helpers.js';
import { defineSection } from '../core/section_registry.js';

defineSection({
  id: 'help/email-support',
  page: 'Email for Help',
  title: 'Email Support',
  subtitle: 'Contact support with server config attached',
  keywords: ['support', 'help', 'email', 'contact', 'config file', 'bug report'],
  navTo: { tab: 'Email for Help', other: '' }
});

registerTab('Email for Help', function(tab, other, prev, $main, $pageHeader) {
  $main.classList.add('page-body--flex-col', 'help-page-body', 'support-shell', 'slab-shell');
  let config = Object.assign({},mist.data);
  delete config.statistics;
  delete config.totals;
  delete config.clients;
  delete config.capabilities;

  config = JSON.stringify(config);
  config = 'Version: '+mist.data.config.version+"\n\nConfig:\n"+config;

  const saveas = {};

  const form = formEngine.buildUI([{
      type: 'help',
      classes: ['page-intro'],
      help: 'You can use this form to email '+APP_NAME+' support if you\'re having difficulties.<br>A copy of your server config file will automatically be included.'
    },{
      type: 'str',
      label: 'Your name',
      validate: ['required'],
      pointer: {
        main: saveas,
        index: 'name'
      },
      value: mist.user.name
    },{
      type: 'email',
      label: 'Your email address',
      validate: ['required'],
      pointer: {
        main: saveas,
        index: 'email'
      }
    },{
      type: 'hidden',
      value: 'Integrated Help',
      pointer: {
        main: saveas,
        index: 'subject'
      }
    },{
      type: 'hidden',
      value: '-',
      pointer: {
        main: saveas,
        index: 'company'
      }
    },{
      type: 'textarea',
      rows: 20,
      label: 'Your message',
      validate: ['required'],
      pointer: {
        main: saveas,
        index: 'message'
      }
    },{
      type: 'textarea',
      rows: 20,
      label: 'Your config file',
      readonly: true,
      value: config,
      pointer: {
        main: saveas,
        index: 'configfile'
      }
    },{
      type: 'buttons',
      buttons: [{
        type: 'save',
        label: 'Send',
        'function': function(me){
          me.textContent = 'Sending..';
          const formData = new URLSearchParams(saveas);
          fetch('https://mistserver.org/contact?skin=plain', {
            method: 'POST',
            headers: {'Content-Type': 'application/x-www-form-urlencoded'},
            body: formData.toString()
          }).then(function(r){ return r.text(); }).then(function(d) {
            const temp = el('span');
            temp.innerHTML = d;
            temp.querySelectorAll('script').forEach(function(s){ s.remove(); });
            $main.innerHTML = temp.innerHTML;
          }).catch(function(){});
        }
      }]
    }]);
  form.classList.add('support-form');
  $main.append(form);
});
