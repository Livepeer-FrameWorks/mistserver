/**
 * Inline entity editor - CRUD list without popups.
 * Each item expands inline as an accordion for editing.
 *
 * @param {Object} options
 * @param {Array} options.items         Array of item objects (mutable reference)
 * @param {Function} options.renderItem  function(item, index) → { title, subtitle, icon, meta }
 *                                       meta: optional DOM element appended after subtitle
 * @param {Function} options.editItem    function(item, index, $editArea, callbacks) - build edit form
 * @param {Function=} options.onAdd      function(callback) - called when Add is clicked;
 *                                       call callback(newItem) to add the item
 * @param {Function=} options.onRemove   function(item, index) - called after removal
 * @param {Function=} options.onReorder  function(items) - called after reorder
 * @param {Function=} options.onChange   function(items) - called after any change
 * @param {boolean=} options.reorderable  Show reorder buttons (default true)
 * @param {boolean=} options.showAdd      Show the Add button (default true)
 * @param {string=} options.emptyMessage  Text when list is empty
 * @param {string=} options.addLabel      Label for the Add button
 * @param {string=} options.addIcon       Icon for the Add button
 * @return {Object} Controller with $el, getItems(), refresh()
 */
import { el } from '../core/dom_helpers.js';

export function inlineEditor(options) {
  let items = options.items || [];
  let expandedIndex = -1;

  const $list = el('div', {class: 'inline-editor'});
  const $empty = el('div', {class: 'inline-editor-empty'}, options.emptyMessage || 'No items configured.');
  const $items = el('div', {class: 'inline-editor-items'});
  const $addBtn = el('button', {
    class: 'inline-editor-add',
    'data-icon': options.addIcon || 'plus'
  }, options.addLabel || 'Add item');

  if (options.showAdd === false) $addBtn.hidden = true;
  $list.appendChild($empty);
  $list.appendChild($items);
  $list.appendChild($addBtn);

  function notifyChange() {
    if (options.onChange) options.onChange(items);
  }

  function renderList() {
    $items.innerHTML = '';
    expandedIndex = -1;

    if (items.length === 0) {
      $empty.hidden = false;
      return;
    }
    $empty.hidden = true;

    for (let i = 0; i < items.length; i++) {
      $items.appendChild(buildItemElement(i));
    }
  }

  function buildItemElement(index) {
    const item = items[index];
    const info = options.renderItem(item, index);

    const $header = el('div', {class: 'inline-editor-item-header'});

    if (info.icon) {
      $header.appendChild(el('span', {class: 'inline-editor-item-icon', 'data-icon': info.icon}));
    }

    $header.appendChild(el('span', {class: 'inline-editor-item-title'}, info.title || ''));

    if (info.subtitle) {
      const $sub = el('span', {class: 'inline-editor-item-subtitle'});
      if (info.subtitle instanceof HTMLElement) {
        $sub.appendChild(info.subtitle);
      } else {
        $sub.textContent = info.subtitle;
      }
      $header.appendChild($sub);
    }

    if (info.meta) {
      $header.appendChild(info.meta);
    }

    const $actions = el('div', {class: 'accordion-actions'});

    if (options.reorderable !== false && items.length > 1) {
      if (index > 0) {
        (function(idx) {
          $actions.appendChild(
            el('button', {
              'data-icon': 'up',
              title: 'Move up',
              onclick: function(e) {
                e.stopPropagation();
                const tmp = items[idx];
                items[idx] = items[idx - 1];
                items[idx - 1] = tmp;
                renderList();
                notifyChange();
                if (options.onReorder) options.onReorder(items);
              }
            })
          );
        })(index);
      }
      if (index < items.length - 1) {
        (function(idx) {
          $actions.appendChild(
            el('button', {
              'data-icon': 'down',
              title: 'Move down',
              onclick: function(e) {
                e.stopPropagation();
                const tmp = items[idx];
                items[idx] = items[idx + 1];
                items[idx + 1] = tmp;
                renderList();
                notifyChange();
                if (options.onReorder) options.onReorder(items);
              }
            })
          );
        })(index);
      }
    }

    (function(idx) {
      $actions.appendChild(
        el('button', {
          class: 'inline-editor-delete',
          'data-icon': 'trash',
          title: 'Remove',
          onclick: function(e) {
            e.stopPropagation();
            if (!window.confirm('Remove this item?')) return;
            const removed = items.splice(idx, 1)[0];
            renderList();
            notifyChange();
            if (options.onRemove) options.onRemove(removed, idx);
          }
        })
      );
    })(index);

    $header.appendChild($actions);

    const $inner = el('div', {class: 'inline-editor-item-inner'});
    const $body = el('div', {class: 'inline-editor-item-body'});
    $body.appendChild($inner);
    const $item = el('div', {class: 'inline-editor-item'});
    $item.appendChild($header);
    $item.appendChild($body);

    let contentBuilt = false;

    (function(idx) {
      $header.addEventListener('click', function(e) {
        if (e.target.closest('.accordion-actions')) return;

        if ($item.classList.contains('expanded')) {
          $item.classList.remove('expanded');
          expandedIndex = -1;
          return;
        }

        const expanded = $items.querySelectorAll('.inline-editor-item.expanded');
        for (let i = 0; i < expanded.length; i++) {
          expanded[i].classList.remove('expanded');
        }

        if (!contentBuilt) {
          contentBuilt = true;
          const editCallbacks = {
            save: function(updatedItem) {
              items[idx] = updatedItem;
              $item.classList.remove('expanded');
              expandedIndex = -1;
              renderList();
              notifyChange();
            },
            cancel: function() {
              $item.classList.remove('expanded');
              expandedIndex = -1;
            }
          };
          options.editItem(items[idx], idx, $inner, editCallbacks);
        }

        $item.classList.add('expanded');
        expandedIndex = idx;
      });
    })(index);

    return $item;
  }

  $addBtn.addEventListener('click', function() {
    if (options.onAdd) {
      options.onAdd(function(newItem) {
        if (newItem) {
          items.push(newItem);
          renderList();
          notifyChange();
        }
      });
    }
  });

  renderList();

  return {
    $el: $list,
    getItems: function() { return items; },
    setItems: function(newItems) {
      items = newItems;
      if (options.items !== newItems) options.items = newItems;
      renderList();
    },
    refresh: renderList,
    getExpandedIndex: function() { return expandedIndex; }
  };
}
