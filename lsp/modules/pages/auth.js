import { APP_NAME, APP_TITLE, LOGIN_TITLE, API_HOST_HELP } from '@brand';
import { registerTab } from '../core/tab_registry.js';
import { apiClient } from '../core/api_client.js';
import { formEngine } from '../core/form_engine.js';
import { navto } from '../core/navigation.js';
import { el } from '../core/dom_helpers.js';
import { parseURL } from '../core/app_state.js';
import { MD5 } from '../../plugins/md5.js';

function appendAuthHeader($card, title, subtitle) {
  const head = el('div', {class: 'login-head'});
  head.appendChild(el('div', {class: 'login-logo'}));
  head.appendChild(el('h2', {class: 'login-title'}, title));

  if (subtitle) {
    const subtitleEl = el('p', {class: 'login-subtitle'});
    if (typeof subtitle === 'string') {
      subtitleEl.innerHTML = subtitle;
    } else {
      subtitleEl.appendChild(subtitle);
    }
    head.appendChild(subtitleEl);
  }

  $card.append(head);
}

registerTab('Login', function(tab, other, prev, $main) {
  if (mist.user.loggedin) {
    navto('Overview');
    return;
  }
  document.title = APP_TITLE;
  UI.elements.menu.classList.add('hide');
  document.body.classList.add('login-mode');
  UI.elements.connection.status.textContent = 'Disconnected';
  UI.elements.connection.status.classList.remove('green');
  UI.elements.connection.status.classList.add('red');

  const $card = el('div', {class: 'login-card login-card--signin'});

  appendAuthHeader(
    $card,
    LOGIN_TITLE,
    'Enter your existing account details to sign in.<br>If this server has not been set up yet, you will be guided through the first-time account setup flow.'
  );

  const $form = formEngine.buildUI([
    {
      label: 'Host',
      help: API_HOST_HELP,
      'default': 'http://localhost:4242/api',
      pointer: {
        main: mist.user,
        index: 'host'
      },
      validate: [function(val,me){
        if (val.slice(0,5) != location.href.slice(0,5)) {
          return {
            msg: "It looks like you're attempting to connect to a host using "+parseURL(val).protocol+", while this page has been loaded over "+parseURL(location.href).protocol+". Your browser may refuse this because it is insecure.",
            "break": false,
            classes: ['orange']
          }
        }
      }]
    },{
      label: 'Username',
      help: 'Please enter your username here.',
      validate_on_change: false,
      validate: ['required'],
      pointer: {
        main: mist.user,
        index: 'name'
      }
    },{
      label: 'Password',
      type: 'password',
      help: 'Please enter your password here.',
      validate_on_change: false,
      validate: ['required'],
      pointer: {
        main: mist.user,
        index: 'rawpassword'
      }
    },{
      type: 'buttons',
      buttons: [{
        label: 'Login',
        type: 'save',
        'function': function(){
          mist.user.password = MD5(mist.user.rawpassword);
          delete mist.user.rawpassword;
          apiClient.send(function(){
            navto('Overview');
          });
        }
      }]
    }
  ]);
  $form.classList.add('auth-form');
  $card.append($form);

  $main.append($card);
});

registerTab('Create a new account', function(tab, other, prev, $main) {
  UI.elements.menu.classList.add('hide');
  document.body.classList.add('login-mode');

  const $card = el('div', {class: 'login-card login-card--create'});

  const subtitle = el('span', {}, 'No account has been created yet at ');
  subtitle.appendChild(el('i', {}, mist.user.host));
  subtitle.appendChild(document.createTextNode('.'));

  appendAuthHeader(
    $card,
    'Create Account',
    subtitle
  );

  const altHostBtn = el('button', {class: 'login-alt-host'}, 'Select other host');
  altHostBtn.addEventListener('click', function() {
    navto('Login');
  });
  $card.append(altHostBtn);

  const $form = formEngine.buildUI([{
      label: 'Desired username',
      type: 'str',
      validate_on_change: false,
      validate: ['required'],
      help: 'Enter your desired username. In the future, you will need this to access the Management Interface.',
      pointer: {
        main: mist.user,
        index: 'name'
      }
    },{
      label: 'Desired password',
      type: 'password',
      validate_on_change: false,
      validate: ['required'],
      help: 'Enter your desired password. In the future, you will need this to access the Management Interface.',
      pointer: {
        main: mist.user,
        index: 'rawpassword'
      },
      classes: ['match_password']
    },{
      label: 'Repeat password',
      type: 'password',
      validate_on_change: false,
      validate: ['required',function(val,me){
        if (!val) { return false; }
        const fields = document.querySelectorAll('.match_password.field');
        let otherField = null;
        for (let i = 0; i < fields.length; i++) {
          if (fields[i] !== me) {
            otherField = fields[i];
            break;
          }
        }
        if (!otherField) { return false; }
        const otherVal = otherField.value;
        if (!otherVal) { return false; }
        if (val != otherVal) {
          return {
            msg:'The fields "Desired password" and "Repeat password" do not match.',
            classes: ['red']
          }
        }
        return false;
      }],
      help: 'Repeat your desired password.',
      classes: ['match_password']
    },{
      type: 'buttons',
      buttons: [{
        type: 'save',
        label: 'Create new account',
        'function': function(){
          apiClient.send(function(){
            navto('Account created');
          },{
            authorize: {
              new_username: mist.user.name,
              new_password: mist.user.rawpassword
            }
          });
          mist.user.password = MD5(mist.user.rawpassword);
          delete mist.user.rawpassword;
        }
      }]
    }
  ]);
  $form.classList.add('auth-form');
  $card.append($form);

  $main.append($card);
});

registerTab('Account created', function(tab, other, prev, $main) {
  UI.elements.menu.classList.add('hide');
  document.body.classList.add('login-mode');

  const $card = el('div', {class: 'login-card login-card--created'});
  let enablingProtocols = false;
  const protocolLog = el('div', {
    class: 'auth-protocol-log',
    'aria-live': 'polite'
  });
  function appendProtocolLog(msg) {
    protocolLog.classList.add('visible');
    protocolLog.appendChild(el('div', {}, msg));
    protocolLog.scrollTop = protocolLog.scrollHeight;
  }

  appendAuthHeader(
    $card,
    'Account Created',
    'Your account has been created successfully.'
  );

  const $form = formEngine.buildUI([
    {
      type: 'text',
      text: 'Would you like to enable all (currently) available protocols with their default settings?'
    },{
      type: 'buttons',
      buttons: [{
        label: 'Enable protocols',
        type: 'save',
        'function': function(){
          if (enablingProtocols) {
            return;
          }
          enablingProtocols = true;
          const enableButton = this;
          enableButton.disabled = true;
          protocolLog.innerHTML = '';
          if (mist.data.config.protocols) {
            appendProtocolLog('Unable to enable all protocols as protocol settings already exist.');
            enablingProtocols = false;
            enableButton.disabled = false;
            return;
          }

          appendProtocolLog('Retrieving available protocols..');
          apiClient.send(function(d){
            const protocols = [];

            for (const i in d.capabilities.connectors) {
              const connector = d.capabilities.connectors[i];

              if (("PUSHONLY" in connector) || ("NODEFAULT" in connector)) {
                continue;
              }

              if (connector.required) {
                appendProtocolLog('Could not enable protocol "'+i+'" because it has required settings.');
                continue;
              }

              protocols.push(
                {connector: i}
              );
              appendProtocolLog('Enabled protocol "'+i+'".');
            }
            appendProtocolLog('Saving protocol settings..');
            apiClient.send(function(d){
              appendProtocolLog('Protocols enabled. Redirecting..');
              setTimeout(function(){
                navto('Overview');
              },1200);

            },{config:{protocols:protocols}});

          },{capabilities:true});
        }
      },{
        label: 'Skip',
        type: 'cancel',
        'function': function(){
          if (enablingProtocols) {
            return;
          }
          navto('Overview');
        }
      }]
    }
  ]);
  $form.classList.add('auth-form', 'auth-form--choices');
  $card.append($form);
  $card.append(protocolLog);

  $main.append($card);
});
