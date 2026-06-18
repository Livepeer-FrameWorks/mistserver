/**
 * Card grid picker with grouped cards.
 * @param {Object} options
 * @param {string=} options.intro      Intro text above cards
 * @param {Array} options.groups       Array of { id, title }
 * @param {Array} options.cards        Array of card descriptors
 * @param {boolean=} options.searchable  Show filter box (default false)
 * @param {boolean=} options.collapsibleGroups  Group headers toggle visibility (default true)
 * @return {HTMLElement}
 *
 * Card descriptor: { id, group, title, desc, icon, available, onClick,
 *                     promo, promoText, promoLink }
 * If available is false and promo is true, card is shown as unavailable promo.
 * If available is false and promo is falsy, card is hidden.
 */
import { el } from '../core/dom_helpers.js';

export function cardPicker(options) {
  const groups = options.groups || [];
  const cards = options.cards || [];
  const collapsibleGroups = options.collapsibleGroups !== false;

  const container = el('div', {class: 'wizard-cards'});

  if (options.intro) {
    container.insertBefore(
      el('p', {class: 'wizard-intro'}, options.intro),
      container.firstChild
    );
  }

  let filter;
  if (options.searchable) {
    filter = el('input', {
      type: 'text',
      placeholder: 'Filter...',
      class: 'wizard-card-filter'
    });
    container.insertBefore(filter, container.firstChild);
  }

  const cardElements = [];
  const groupElements = [];

  for (let g = 0; g < groups.length; g++) {
    const group = groups[g];
    const groupCards = [];

    for (let c = 0; c < cards.length; c++) {
      if (cards[c].group !== group.id) continue;

      const card = cards[c];
      const isAvailable = card.available !== false;
      const isPromo = !isAvailable && card.promo;

      if (!isAvailable && !isPromo) continue;

      let cardEl;
      if (isPromo) {
        cardEl = el('div', {class: 'wizard-card promo'});
      } else {
        cardEl = el('button', {class: 'wizard-card'});
      }

      if (card.icon) {
        cardEl.setAttribute('data-icon', card.icon);
      }

      cardEl.appendChild(
        el('div', {class: 'wizard-card-title'}, card.title)
      );

      if (card.desc) {
        cardEl.appendChild(el('div', {class: 'wizard-card-desc'}, card.desc));
      }

      if (isPromo && card.promoText) {
        cardEl.appendChild(el('div', {class: 'wizard-card-desc'}, card.promoText));
        if (card.promoLink) {
          cardEl.appendChild(
            el('a', {
              class: 'wizard-card-link',
              href: card.promoLink,
              target: '_blank'
            }, 'Learn more')
          );
        }
      }

      if (card.selected) {
        cardEl.classList.add('selected');
      }

      if (!isPromo && card.onClick) {
        (function(cb, cardId) {
          cardEl.addEventListener('click', function() { cb(cardId); });
        })(card.onClick, card.id);
      }

      cardEl._cardId = card.id;
      cardEl._cardTitle = (card.title || '').toLowerCase();
      cardEl._cardDesc = (card.desc || '').toLowerCase();
      cardEl._filterMatch = true;
      groupCards.push(cardEl);
      cardElements.push(cardEl);
    }

    if (groupCards.length === 0) continue;

    let groupTitle;
    if (collapsibleGroups) {
      groupTitle = el('button', {
        class: 'wizard-group-title',
        type: 'button'
      }, [
        el('span', {class: 'wizard-group-title-text'}, group.title),
        el('span', {class: 'wizard-group-title-count'}, groupCards.length.toString())
      ]);
    } else {
      groupTitle = el('div', {class: 'wizard-group-title'}, group.title);
    }
    const groupCardsContainer = el('div', {class: 'wizard-group-cards'}, groupCards);

    const groupEl = el('div', {class: 'wizard-group'}, [groupTitle, groupCardsContainer]);
    if (collapsibleGroups) {
      groupEl.setAttribute('data-default-collapsed', 'no');
      (function(groupRef, titleRef) {
        titleRef.addEventListener('click', function() {
          groupRef.classList.toggle('collapsed');
          groupRef.setAttribute('data-default-collapsed', groupRef.classList.contains('collapsed') ? 'yes' : 'no');
        });
      })(groupEl, groupTitle);
    }

    groupElements.push({
      $group: groupEl,
      $cards: groupCardsContainer
    });
    container.appendChild(groupEl);
  }

  if (filter) {
    filter.addEventListener('input', function() {
      const term = this.value.toLowerCase().trim();
      for (let i = 0; i < cardElements.length; i++) {
        const cardEl = cardElements[i];
        let match = true;
        if (!term) {
          match = true;
        } else {
          match = cardEl._cardTitle.indexOf(term) >= 0 ||
                  cardEl._cardDesc.indexOf(term) >= 0;
        }
        cardEl._filterMatch = match;
        cardEl.hidden = !match;
      }

      for (let g = 0; g < groupElements.length; g++) {
        const entry = groupElements[g];
        const anyVisible = Array.from(entry.$cards.children).filter(function(child) {
          return child._filterMatch !== false;
        }).length > 0;
        entry.$group.hidden = !anyVisible;
        if (!collapsibleGroups) continue;
        if (term && anyVisible) {
          entry.$group.classList.remove('collapsed');
        } else if (!term) {
          const collapsed = entry.$group.getAttribute('data-default-collapsed') === 'yes';
          if (collapsed) {
            entry.$group.classList.add('collapsed');
          } else {
            entry.$group.classList.remove('collapsed');
          }
        }
      }
    });
  }

  container.$el = container;
  container.$cards = cardElements;
  return container;
}
