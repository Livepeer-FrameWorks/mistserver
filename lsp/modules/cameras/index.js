/**
 * Cameras module - registers sub-tab handlers for Camera Detail, PTZ Control, and Multiview.
 * The main Devices tab is registered via ModeDispatch in camera_list.js.
 */
import { registerTab } from '../core/tab_registry.js';
import { CameraDetail } from './camera_detail.js';
import { PTZControl } from './ptz_control.js';
import './multiview.js';

registerTab('Camera Detail', function(tab, other, prev, $main, $pageHeader) {
  $main.classList.add('page-body--flex-col', 'camera-detail-page-body');
  $pageHeader.classList.add('camera-tab-header');
  const component = new CameraDetail();
  component.render($main, {other: other, $pageHeader: $pageHeader});
});

registerTab('PTZ Control', function(tab, other, prev, $main, $pageHeader) {
  $main.classList.add('page-body--flex-col', 'ptz-page-body');
  $pageHeader.classList.add('camera-tab-header');
  const component = new PTZControl();
  component.render($main, {other: other, $pageHeader: $pageHeader});
});
