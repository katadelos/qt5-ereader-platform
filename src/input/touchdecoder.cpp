#include "touchdecoder.h"

#include <linux/input.h>

void TouchDecoder::configure(const TouchCapabilities& capabilities, const TouchSnapshot& snapshot) {
    m_capabilities = capabilities;
    reset(snapshot);
}

void TouchDecoder::reset(const TouchSnapshot& snapshot) {
    m_contacts = snapshot.contacts;
    if (m_capabilities.protocol != TouchProtocol::TypeA) {
        m_contacts.resize(m_capabilities.slotCount);
    }
    m_slots.current = snapshot.currentSlot;
    m_touchKeyDown = snapshot.touchKeyDown;
    m_discardUntilRelease = snapshot.discardUntilRelease;
    m_frameDirty = false;
    m_typeA = {};
    m_single = {};
    if (m_capabilities.protocol == TouchProtocol::Single && !m_contacts.isEmpty()) {
        m_single.position = QPoint(m_contacts[0].x, m_contacts[0].y);
        m_single.lastPosition = m_single.position;
    }
}

bool TouchDecoder::handleEvent(const input_event& event) {
    switch (m_capabilities.protocol) {
    case TouchProtocol::Single:
        handleSingleTouchEvent(event);
        break;
    case TouchProtocol::TypeA:
        handleTypeAEvent(event);
        break;
    case TouchProtocol::TypeB:
        handleSlotEvent(event);
        break;
    }
    if (event.type != EV_SYN || event.code != SYN_REPORT) {
        return false;
    }
    const bool ready = m_frameDirty || m_capabilities.protocol == TouchProtocol::TypeA;
    m_frameDirty = false;
    if (ready && m_capabilities.protocol == TouchProtocol::Single &&
        m_contacts[0].state != Qt::TouchPointReleased) {
        m_single.lastPosition = QPoint(m_contacts[0].x, m_contacts[0].y);
    }
    return ready;
}

void TouchDecoder::handleSlotEvent(const input_event& event) {
    if (event.type == EV_ABS) {
        handleSlotAbsoluteEvent(event);
    } else if (event.type == EV_KEY && event.code == BTN_TOUCH) {
        m_touchKeyDown = event.value != 0;
        if (!m_touchKeyDown) {
            // Some streams report the final release through BTN_TOUCH even
            // when no ABS_MT_TRACKING_ID transition reaches userspace.
            for (TouchContact& contact : m_contacts) {
                if (contact.active) {
                    contact.state = Qt::TouchPointReleased;
                    m_frameDirty = true;
                }
            }
        }
        m_frameDirty |= m_discardUntilRelease;
    }
}

void TouchDecoder::handleSlotAbsoluteEvent(const input_event& event) {
    if (event.code == TouchEvent::Slot) {
        m_slots.current =
            event.value >= 0 && event.value < m_capabilities.slotCount ? event.value : -1;
        return;
    }
    if (m_slots.current < 0) {
        return;
    }
    if (event.code == ABS_MT_TRACKING_ID) {
        TouchContact& contact = m_contacts[m_slots.current];
        if (event.value < 0) {
            if (!contact.active) {
                return;
            }
            contact.state = Qt::TouchPointReleased;
            contact.active = true;
        } else if (!contact.active || contact.trackingId != event.value) {
            contact.trackingId = event.value;
            contact.state = Qt::TouchPointPressed;
            contact.active = true;
        } else {
            // Elipsa 2E's kernel repeats unchanged slot identifiers.
            // The same contact must remain a move/stationary event.
            return;
        }
        m_frameDirty = true;
    } else if (event.code == ABS_MT_POSITION_X) {
        TouchContact& contact = m_contacts[m_slots.current];
        contact.x = event.value;
        contact.xKnown = true;
        if (contact.active && contact.state != Qt::TouchPointPressed &&
            contact.state != Qt::TouchPointReleased) {
            contact.state = Qt::TouchPointMoved;
        }
        m_frameDirty |= contact.active;
    } else if (event.code == ABS_MT_POSITION_Y) {
        TouchContact& contact = m_contacts[m_slots.current];
        contact.y = event.value;
        contact.yKnown = true;
        if (contact.active && contact.state != Qt::TouchPointPressed &&
            contact.state != Qt::TouchPointReleased) {
            contact.state = Qt::TouchPointMoved;
        }
        m_frameDirty |= contact.active;
    } else if (event.code == TouchEvent::Pressure) {
        m_contacts[m_slots.current].pressure = event.value;
        m_frameDirty |= m_contacts[m_slots.current].active;
    }
}

void TouchDecoder::handleSingleTouchEvent(const input_event& event) {
    TouchContact& contact = m_contacts[0];
    if (event.type == EV_ABS) {
        if (event.code == ABS_X) {
            contact.x = event.value;
            m_single.position.setX(event.value);
        } else if (event.code == ABS_Y) {
            contact.y = event.value;
            m_single.position.setY(event.value);
        } else if (event.code == ABS_PRESSURE) {
            contact.pressure = event.value;
        } else {
            return;
        }
        if (contact.active) {
            if (contact.state != Qt::TouchPointPressed && contact.state != Qt::TouchPointReleased) {
                contact.state = Qt::TouchPointMoved;
            }
            m_frameDirty = true;
        }
    } else if (event.type == EV_KEY && event.code == BTN_TOUCH) {
        // This zForce driver sends its saved *unrotated* coordinates on up,
        // although down/move use the customer-UI rotation. End the contact
        // where the last valid frame put it, without a spurious release jump.
        if ((event.value == 0) && m_capabilities.releaseAtLastPosition) {
            contact.x = m_single.lastPosition.x();
            contact.y = m_single.lastPosition.y();
        }
        contact.trackingId = 0;
        contact.active = true;
        contact.state = (event.value != 0) ? Qt::TouchPointPressed : Qt::TouchPointReleased;
        m_frameDirty = true;
    }
}

void TouchDecoder::handleTypeAEvent(const input_event& event) {
    if (event.type == EV_ABS) {
        if (event.code == ABS_MT_TRACKING_ID) {
            m_typeA.pending.trackingId = event.value;
            m_typeA.pending.active = event.value >= 0;
        } else if (event.code == ABS_MT_POSITION_X) {
            m_typeA.pending.x = event.value;
        } else if (event.code == ABS_MT_POSITION_Y) {
            m_typeA.pending.y = event.value;
        } else if (event.code == TouchEvent::Pressure) {
            m_typeA.pending.pressure = event.value;
        }
    } else if (event.type == EV_KEY && event.code == BTN_TOUCH) {
        m_touchKeyDown = event.value != 0;
        if (!m_touchKeyDown) {
            m_typeA.pending = TouchContact();
            m_typeA.contacts.clear();
        }
    } else if (event.type == EV_SYN && event.code == SYN_MT_REPORT) {
        finishTypeAContact();
    } else if (event.type == EV_SYN && event.code == SYN_REPORT) {
        finishTypeAContact();
        finishTypeAFrame();
    }
}

void TouchDecoder::finishTypeAContact() {
    if (!m_typeA.pending.active) {
        m_typeA.pending = TouchContact();
        return;
    }
    for (TouchContact& contact : m_typeA.contacts) {
        if (contact.trackingId == m_typeA.pending.trackingId) {
            contact = m_typeA.pending;
            m_typeA.pending = TouchContact();
            return;
        }
    }
    m_typeA.contacts.append(m_typeA.pending);
    m_typeA.pending = TouchContact();
}

void TouchDecoder::finishTypeAFrame() {
    // Type A frames contain the complete set of contacts, not slot deltas.
    // Keep Qt IDs stable when packet order changes and release contacts that
    // disappear even if BTN_TOUCH stays down for another finger.
    const QVector<TouchContact> previous = m_contacts;
    m_contacts = m_typeA.contacts;
    m_typeA.contacts.clear();
    const int activeCount = m_contacts.size();
    for (TouchContact& contact : m_contacts) {
        contact.state = Qt::TouchPointPressed;
        for (const TouchContact& old : previous) {
            if (old.trackingId == contact.trackingId) {
                const bool unchanged =
                    old.x == contact.x && old.y == contact.y && old.pressure == contact.pressure;
                contact.state = unchanged ? Qt::TouchPointStationary : Qt::TouchPointMoved;
                break;
            }
        }
    }
    for (TouchContact old : previous) {
        bool present = false;
        for (int index = 0; index < activeCount; ++index) {
            if (m_contacts[index].trackingId == old.trackingId) {
                present = true;
                break;
            }
        }
        if (!present) {
            old.state = Qt::TouchPointReleased;
            m_contacts.append(old);
        }
    }
}

QVector<TouchContact> TouchDecoder::takeFrame() {
    QVector<TouchContact> frame;
    bool anyContactDown = false;
    for (TouchContact& contact : m_contacts) {
        if (!contact.active) {
            continue;
        }
        const bool released = contact.state == Qt::TouchPointReleased;
        anyContactDown |= !released;
        // Type B axes persist between contacts. Unknown axes remain unknown
        // until the kernel actually reports them after recovery.
        if (m_capabilities.protocol != TouchProtocol::TypeB || (contact.xKnown && contact.yKnown)) {
            frame.append(contact);
            if (!released) {
                contact.state = Qt::TouchPointStationary;
            }
        }
        if (released) {
            if (m_capabilities.protocol == TouchProtocol::Single) {
                // zForce release coordinates differ from the last visible
                // position. Retain the raw axes for the next event stream.
                contact.x = m_single.position.x();
                contact.y = m_single.position.y();
            }
            contact.active = false;
            contact.trackingId = -1;
        }
    }
    if (m_capabilities.protocol == TouchProtocol::TypeA) {
        for (int index = m_contacts.size() - 1; index >= 0; --index) {
            if (!m_contacts[index].active) {
                m_contacts.removeAt(index);
            }
        }
    }
    if (m_discardUntilRelease) {
        if (!anyContactDown && !m_touchKeyDown) {
            m_discardUntilRelease = false;
        }
        return {};
    }
    return frame;
}
