// Bootstrap: globals needed before anything else
import './core/dom_helpers.js';
import './core/app_state.js';
import './core/tab_registry.js';

// Core: Level 0
import './core/navigation.js';
import './core/clipboard.js';
import './core/capabilities.js';
import './core/ui_core.js';
import './streams/stream_hints.js';
import './core/formatters.js';
import './core/constants.js';

// Core: Level 1
import './core/sockets.js';
import './core/mist_helpers.js';
import './core/dynamic.js';
import './core/tab_view.js';
import './core/form_engine.js';
import './core/api_client.js';
import './core/appshell.js';
import './core/themes.js';
import './core/mode_dispatch.js';
import './core/ui_helpers.js';
import './core/icons.js';

// Fun
import './fun/duck_swarm.js';

// Stream domain
import './streams/stream_utils.js';
import './streams/streamkeys.js';
import './streams/stream_save.js';
import './streams/stream_scenarios.js';
import './streams/process_templates.js';

// Push domain
import './pushes/push_renderers.js';
import './pushes/pushes.js';
import './pushes/push_scenarios.js';

// Search
import './core/section_registry.js';
import './core/search_ui.js';

// Pages
import './pages/variables.js';
import './pages/auth.js';
import './pages/overview.js';
import './pages/general.js';
import './pages/protocols.js';
import './pages/chart_helpers.js';
import './pages/dashboard.js';
import './pages/connections.js';
import './pages/help.js';
import './pages/triggers.js';

// Components
import './components/wizard.js';
import './components/card_picker.js';
import './components/accordion_tree.js';
import './components/disclosure_form.js';
import './components/inline_editor.js';
import './components/composer.js';

// Stream pages
import './streams/streams.js';
import './streams/stream_config.js';

// Push pages
import './pushes/push.js';
import './pushes/push_config.js';

// Cameras
import './cameras/component.js';
import './cameras/api.js';
import './cameras/dom.js';
import './cameras/camera_list.js';
import './cameras/camera_detail.js';
import './cameras/ptz_control.js';
import './cameras/stream_dialog.js';
import './cameras/index.js';

// Init: must be last (registers DOMContentLoaded + hashchange handlers)
import './core/init.js';
