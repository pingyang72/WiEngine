﻿/*
 * Copyright (c) 2010 WiYun Inc.
 * Author: luma(stubma@gmail.com)
 *
 * For all entities this program is free software; you can redistribute
 * it and/or modify it under the terms of the 'WiEngine' license with
 * the additional provision that 'WiEngine' must be credited in a manner
 * that can be be observed by end users, for example, in the credits or during
 * start up. (please find WiEngine logo in sdk's logo folder)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdlib.h>
#include "wyDirector.h"
#include "wyNode.h"
#include "wyTexture2D.h"
#include <pthread.h>
#include "wyUtils.h"
#include "wyActionManager.h"
#include "wyEventDispatcher.h"
#include "wyLog.h"
#include "wyMaterial.h"
#include "wyMesh.h"
#include "wyParallaxObject.h"
#include "wyMaterial.h"
#include "wyAnimation.h"
#include "wyScheduler.h"
#include "wyCamera.h"
#include "wyGridController.h"

extern pthread_mutex_t gMutex;

extern wyScheduler* gScheduler;
extern wyEventDispatcher* gEventDispatcher;
extern wyActionManager* gActionManager;
extern wyDirector* gDirector;

bool wyNode::activateTimer(wyArray* arr, void* ptr, int index, void* data) {
	wyTimer* t = (wyTimer*)ptr;
	gScheduler->scheduleLocked(t);

	return true;
}

bool wyNode::deactivateTimer(wyArray* arr, void* ptr, int index, void* data) {
	wyTimer* t = (wyTimer*)ptr;
	gScheduler->unscheduleLocked(t);

	return true;
}

bool wyNode::releaseTimer(wyArray* arr, void* ptr, int index, void* data) {
	wyObjectRelease((wyObject*)ptr);
	return true;
}

bool wyNode::zOrderLocator(void* elementPtr, void* ptr, void* data) {
	return ((wyNode*)elementPtr)->getZOrder() > *(int*)data;
}

bool wyNode::tagLocator(void* elementPtr, void* ptr, void* data) {
	return ((wyNode*)elementPtr)->getTag() == *(int*)data;
}

bool wyNode::sOnEnter(wyArray* arr, void* ptr, int index, void* data) {
	wyNode* n = (wyNode*)ptr;
	n->onEnter();
	return true;
}

bool wyNode::sOnExit(wyArray* arr, void* ptr, int index, void* data) {
	wyNode* n = (wyNode*)ptr;
	n->onExit();
	return true;
}

bool wyNode::sOnEnterTransitionDidFinish(wyArray* arr, void* ptr, int index, void* data) {
	wyNode* n = (wyNode*)ptr;
	n->onEnterTransitionDidFinish();
	return true;
}

bool wyNode::sCleanup(wyArray* arr, void* ptr, int index, void* data) {
	wyNode* n = (wyNode*)ptr;
	n->cleanup();
	return true;
}

void wyNode::deactivateTimers() {
	if(m_timers != NULL)
		wyArrayEach(m_timers, deactivateTimer, NULL);

	gActionManager->pauseActions(this, false);
}

void wyNode::activateTimers() {
	if(m_timers != NULL)
		wyArrayConcurrentEach(m_timers, activateTimer, NULL);

	gActionManager->resumeActions(this, false);
}

int wyNode::reorderChild(wyNode* child, int z) {
	// if child list is locked, means something is happening
	if(m_children->locked || m_childrenChanging)
		return -1;
	m_childrenChanging = true;

	// remove it first
	int index = wyArrayIndexOf(m_children, child, NULL, NULL);
	if(index != -1) {
		child->m_parent = NULL;
		wyArrayDeleteIndex(m_children, index);
	} else {
		LOGW("wyNode::reorderChild: you want to reorder a child whose parent is not this?");
		return -1;
	}

	// insert it back with new z-order
	int ret = insertChild(child, z);

	// reset flag
	m_childrenChanging = false;

	return ret;
}

int wyNode::insertChild(wyNode* n, int z) {
	// insert or push
	int index = wyArrayIndexOf(m_children, n, zOrderLocator, &z);
	if(index == -1)
		wyArrayPush(m_children, n);
	else
		wyArrayInsert(m_children, n, index);

	// save z order and parent
	n->m_zOrder = z;
	n->m_parent = this;

	return index;
}

void wyNode::addChild(wyNode* child, int z, int tag) {
	// check child
	if(child == NULL) {
		LOGW("Can't add a NULL child");
		return;
	}

	// check parent
	if(child->getParent() != NULL) {
		LOGW("The child(%s, %d) is already attached to another parent(%s, %d), skip addChild",
				child->getClassName(), child, child->getParent()->getClassName(), child->getParent());
		return;
	}

	// if child list is locked, means something is happening
	if(m_children->locked || m_childrenChanging)
		return;
	m_childrenChanging = true;

	// insert and execute onEnter if running
	insertChild(child, z);
	wyObjectRetain(child);
	child->m_tag = tag;

	// callback, child has a chance to do something
	child->onAttachToParent(this);

	// enter if running
	if(m_running) {
		child->onEnter();
	}

	m_childrenChanging = false;
}

wyNode* wyNode::getChildByTag(int tag) {
	int index = wyArrayIndexOf(m_children, NULL, tagLocator, &tag);
	if(index != -1)
		return (wyNode*)wyArrayGet(m_children, index);
	else
		return NULL;
}

wyNode** wyNode::getChildrenByTag(int tag, size_t* count) {
	// find all children
	wyNode** ret = (wyNode**)wyMalloc(sizeof(wyNode*) * m_children->num);
	int j = 0;
	for(int i = 0; i < m_children->num; i++) {
		wyNode* child = (wyNode*)wyArrayGet(m_children, i);
		if(child->getTag() == tag)
			ret[j++] = child;
	}

	// save count
	if(count)
		*count = j;

	return ret;
}

void wyNode::removeChild(wyNode* child, bool cleanup) {
	// if child list is locked, means something is happening
	if(m_children->locked || m_childrenChanging)
		return;
	m_childrenChanging = true;
	
	// try to find this child
	int index = wyArrayIndexOf(m_children, child, NULL, NULL);

	// if found, remove it
	if(index != -1) {
		child->onDetachFromParent(this);

		if(m_running)
			child->onExit();

		if(cleanup)
			child->cleanup();

		child->m_parent = NULL;
		wyArrayDeleteIndex(m_children, index);
		wyObjectRelease(child);
	}

	// restore flag
	m_childrenChanging = false;
}

wyRect wyNode::getResolvedClipRect() {
	// get clip rect relative to base size
	wyRect r = m_clipRect;

	// if relative to self, add offset of self
	if(m_clipRelativeToSelf) {
		// get clip rect relative to base size
		wyRect bound = getBoundingBoxRelativeToWorld();
		r.x += bound.x;
		r.y += bound.y;
	}

	// get clip rect relative to real size
	if(wyDevice::scaleMode != SCALE_MODE_BY_DENSITY)
		r = getBaseSizeClipRect(r);

	return r;
}

void wyNode::onEnter() {
	if(!m_running) {
		if(m_touchEnabled)
			gEventDispatcher->addTouchHandlerLocked(this, m_touchPriority);

		if(m_keyEnabled)
			gEventDispatcher->addKeyHandlerLocked(this, m_keyPriority);
	
		if(m_gestureEnabled)
			gEventDispatcher->addGestureHandlerLocked(this, m_gesturePriority);

		if(m_doubleTabEnabled)
			gEventDispatcher->addDoubleTapHandlerLocked(this, m_doubleTapPriority);

		if(m_accelerometerEnabled)
			gEventDispatcher->addAccelHandlerLocked(this, m_accelerometerPriority);

		wyArrayEach(m_children, sOnEnter, NULL);
		activateTimers();
		m_running = true;
	}
}

void wyNode::onExit() {
	if(m_running) {
		if(m_touchEnabled)
			gEventDispatcher->removeTouchHandlerLocked(this);

		if(m_keyEnabled)
			gEventDispatcher->removeKeyHandlerLocked(this);

		if(m_accelerometerEnabled)
			gEventDispatcher->removeAccelHandlerLocked(this);

		if(m_gestureEnabled)
			gEventDispatcher->removeGestureHandlerLocked(this);

		if(m_doubleTabEnabled)
			gEventDispatcher->removeDoubleTapHandlerLocked(this);

		deactivateTimers();
		m_running = false;

		// make children exit
		wyArrayEach(m_children, sOnExit, NULL);
	}
}

void wyNode::onEnterTransitionDidFinish() {
	wyArrayEach(m_children, sOnEnterTransitionDidFinish, NULL);
}

void wyNode::onAttachToParent(wyNode* parent) {
}

void wyNode::onDetachFromParent(wyNode* parent) {
}

void wyNode::cleanup() {
	// stop all actions
	stopAllActions(false);

	// clear selectors
	if(m_timers != NULL) {
		wyArrayEach(m_timers, releaseTimer, NULL);
		wyArrayClear(m_timers);
	}

	// cleanup children
	wyArrayEach(m_children, sCleanup, NULL);
}

wyNode::~wyNode() {
	// nullify parent
	m_parent = NULL;

	// remove from physics node queue
	if(m_velocityX != 0 || m_velocityY != 0 || m_accelerationX != 0 || m_accelerationY != 0) {
		if(gActionManager != NULL)
			gActionManager->removePhysicsNode(this);
	}

	// release render pairs
	for(vector<RenderPair>::iterator iter = m_renderPairs->begin(); iter != m_renderPairs->end(); iter++) {
		wyObjectRelease(iter->material);
		wyObjectRelease(iter->mesh);
	}
	WYDELETE(m_renderPairs);

	// release members
	wyObjectRelease(m_camera);
	wyObjectRelease(m_grid);
	wyObjectRelease(m_downSelector);
	wyObjectRelease(m_upSelector);
	wyObjectRelease(m_moveOutSelector);
	removeAllChildrenLocked(true);
	wyArrayDestroy(m_children);
	m_children = NULL;
	if(m_timers != NULL) {
		wyArrayEach(m_timers, releaseTimer, NULL);
		wyArrayDestroy(m_timers);
		m_timers = NULL;
	}
}

void wyNode::setAnchor(float x, float y) {
	if(m_anchorX != x || m_anchorY != y) {
		m_anchorX = x;
		m_anchorY = y;
		m_anchorPointX = m_width * x;
		m_anchorPointY = m_height * y;
		setTransformDirty();
	}
}

void wyNode::setAnchorX(float x) {
	if(m_anchorX != x) {
		m_anchorX = x;
		m_anchorPointX = m_width * x;
		setTransformDirty();
	}
}

void wyNode::setAnchorY(float y) {
	if(m_anchorY != y) {
		m_anchorY = y;
		m_anchorPointY = m_height * y;
		setTransformDirty();
	}
}

void wyNode::moveAnchorTo(float x, float y) {
	float newX = m_positionX;
	float newY = m_positionY;

	// if relative to anchor point, need offset
	if(m_relativeAnchorPoint) {
		newX += m_width * (x - m_anchorX);
		newY += m_height * (y - m_anchorY);
	}

	// consider scale
	if(m_scaleX != 1) {
		newX += (1 - m_scaleX) * (m_anchorX - x) * m_width;
	}
	if(m_scaleY != 1) {
		newY += (1 - m_scaleY) * (m_anchorY - y) * m_height;
	}

	// set new position and anchor
	setPosition(newX, newY);
	setAnchor(x, y);
}

void wyNode::setContentSize(float w, float h) {
	if(m_width != w || m_height != h) {
		m_width = w;
		m_height = h;
		m_anchorPointX = w * m_anchorX;
		m_anchorPointY = h * m_anchorY;
		setTransformDirty();
		setNeedUpdateMesh(true);
	}
}

void wyNode::setPosition(float x, float y) {
	m_positionX = x;
	m_positionY = y;
	setTransformDirty();

	if(m_positionCallback) {
		m_positionCallback->onNodePositionChanged(this);
	} 
}

void wyNode::setRotation(float rot) {
	if(rot != m_rotation) {
		m_rotation = rot;
		setTransformDirty();
	}
}

void wyNode::setScale(float scale) {
	if(scale != m_scaleX || scale != m_scaleY) {
		m_scaleX = m_scaleY = scale;
		setTransformDirty();
	}
}

void wyNode::setScaleX(float scaleX) {
	if(scaleX != m_scaleX) {
		m_scaleX = scaleX;
		setTransformDirty();
	}
}

void wyNode::setScaleY(float scaleY) {
	if(scaleY != m_scaleY) {
		m_scaleY = scaleY;
		setTransformDirty();
	}
}

void wyNode::setSkewX(float degree) {
	if(degree != m_skewX) {
		m_skewX = degree;
		setTransformDirty();
	}
}

void wyNode::setSkewY(float degree) {
	if(degree != m_skewY) {
		m_skewY = degree;
		setTransformDirty();
	}
}

void wyNode::setAcceleration(float ax, float ay) {
	m_accelerationX = ax;
	m_accelerationY = ay;

	if(m_velocityX != 0 || m_velocityY != 0 || m_accelerationX != 0 || m_accelerationY != 0) {
		if(gActionManager != NULL)
			gActionManager->addPhysicsNode(this);
	} else {
		if(gActionManager != NULL)
			gActionManager->removePhysicsNode(this);
	}
}

void wyNode::setAccelerationX(float ax) {
	m_accelerationX = ax;

	if(m_velocityX != 0 || m_velocityY != 0 || m_accelerationX != 0 || m_accelerationY != 0) {
		if(gActionManager != NULL)
			gActionManager->addPhysicsNode(this);
	} else {
		if(gActionManager != NULL)
			gActionManager->removePhysicsNode(this);
	}
}

void wyNode::setAccelerationY(float ay) {
	m_accelerationY = ay;

	if(m_velocityX != 0 || m_velocityY != 0 || m_accelerationX != 0 || m_accelerationY != 0) {
		if(gActionManager != NULL)
			gActionManager->addPhysicsNode(this);
	} else {
		if(gActionManager != NULL)
			gActionManager->removePhysicsNode(this);
	}
}

void wyNode::setVelocity(float vx, float vy) {
	m_velocityX = vx;
	m_velocityY = vy;

	if(m_velocityX != 0 || m_velocityY != 0 || m_accelerationX != 0 || m_accelerationY != 0) {
		if(gActionManager != NULL)
			gActionManager->addPhysicsNode(this);
	} else {
		if(gActionManager != NULL)
			gActionManager->removePhysicsNode(this);
	}
}

void wyNode::setVelocityX(float vx) {
	m_velocityX = vx;

	if(m_velocityX != 0 || m_velocityY != 0 || m_accelerationX != 0 || m_accelerationY != 0) {
		if(gActionManager != NULL)
			gActionManager->addPhysicsNode(this);
	} else {
		if(gActionManager != NULL)
			gActionManager->removePhysicsNode(this);
	}
}

void wyNode::setVelocityY(float vy) {
	m_velocityY = vy;

	if(m_velocityX != 0 || m_velocityY != 0 || m_accelerationX != 0 || m_accelerationY != 0) {
		if(gActionManager != NULL)
			gActionManager->addPhysicsNode(this);
	} else {
		if(gActionManager != NULL)
			gActionManager->removePhysicsNode(this);
	}
}

wyRect wyNode::getBaseSizeClipRect(wyRect r) {
	switch(wyDevice::scaleMode) {
		case SCALE_MODE_BASE_SIZE_FIT_XY:
			r.x *= wyDevice::baseScaleX;
			r.y *= wyDevice::baseScaleY;
			r.width *= wyDevice::baseScaleX;
			r.height *= wyDevice::baseScaleY;
			break;
	}
	return r;
}

void wyNode::setClipRect(wyRect clip, bool relativeToSelf) {
	m_hasClip = true;
	m_clipRect = clip;
	m_clipRelativeToSelf = relativeToSelf;
}

void wyNode::removeChildLocked(wyNode* child, bool cleanup) {
	pthread_mutex_lock(&gMutex);
	if(child->m_parent == this) {
		removeChild(child, cleanup);
	}
	pthread_mutex_unlock(&gMutex);
}

void wyNode::removeChildByTagLocked(int tag, bool cleanup) {
	pthread_mutex_lock(&gMutex);
	wyNode* child = getChildByTag(tag);
	if(child != NULL) {
		removeChild(child, cleanup);
	}
	pthread_mutex_unlock(&gMutex);
}

void wyNode::removeChildrenByTagLocked(int tag, bool cleanup) {
	pthread_mutex_lock(&gMutex);

	size_t count;
	wyNode** children = getChildrenByTagLocked(tag, &count);
	for(size_t i = 0; i < count; i++) {
		removeChild(children[i], cleanup);
	}

	pthread_mutex_unlock(&gMutex);
}

void wyNode::removeAllChildren(bool cleanup) {
	// if child list is locked, means something is happening
	if(m_children->locked || m_childrenChanging)
		return;
	m_childrenChanging = true;

	for(int i = 0; i < m_children->num; i++) {
		wyNode* child = (wyNode*)wyArrayGet(m_children, i);

		child->onDetachFromParent(this);

		if(m_running)
			child->onExit();

		if(cleanup)
			child->cleanup();

		child->m_parent = NULL;
		wyObjectRelease(child);
	}
	wyArrayClear(m_children);

	// reset flag
	m_childrenChanging = false;
}

void wyNode::removeAllChildrenLocked(bool cleanup) {
	pthread_mutex_lock(&gMutex);
	removeAllChildren(cleanup);
	pthread_mutex_unlock(&gMutex);
}

bool wyNode::hitTest(float x, float y) {
	wyRect rect = getBoundingBoxRelativeToWorld();
	if(m_touchCoffin) {
		wyRect r2 = m_touchCoffin->getBoundingBoxRelativeToWorld();
		rect = wyrIntersect(rect, r2);
	}
	return wyrContains(rect, wyp(x, y));
}

bool wyNode::isVisibleFromRoot() {
	if(!m_visible)
		return false;

	for(wyNode* p = m_parent; p != NULL; p = p->getParent()) {
		if(!p->isVisible())
			return false;
	}
	return true;
}

bool wyNode::isEnabledFromRoot() {
	if(!m_enabled)
		return false;

	for(wyNode* p = m_parent; p != NULL; p = p->getParent()) {
		if(!p->isEnabled())
			return false;
	}
	return true;
}

// accelerometer
void wyNode::accelerometerChanged(float accelX, float accelY, float accelZ) {
}

bool wyNode::keyDown(wyKeyEvent& e) {
	return false;
}

bool wyNode::keyUp(wyKeyEvent& e) {
	return false;
}

bool wyNode::keyMultiple(wyKeyEvent& e) {
	return false;
}

bool wyNode::touchesBegan(wyMotionEvent& e) {
	setSelected(true);
	if(m_downSelector != NULL) {
		m_downSelector->invoke();
	}
	return m_interceptTouch;
}

bool wyNode::touchesMoved(wyMotionEvent& e) {
	// check any touch in node?
	bool inside = false;
	for(int i = 0; i < e.pointerCount; i++) {
		if(hasPid(e.pid[i])) {
			if(hitTest(e.x[i], e.y[i])) {
				inside = true;
				break;
			}
		}
	}

	// set selected status
	setSelected(inside);

	// if no touch in node, call move out selector
	if(!inside) {
		if(m_moveOutSelector != NULL) {
			if (m_moveOutSelector != NULL) {
				m_moveOutSelector->invoke();
			}
		}
	}

	return m_interceptTouch;
}

bool wyNode::touchesEnded(wyMotionEvent& e) {
	setSelected(false);
	if (m_upSelector != NULL) {
		m_upSelector->invoke();
	}
	return m_interceptTouch;
}

bool wyNode::touchesCancelled(wyMotionEvent& e) {
	setSelected(false);
	return m_interceptTouch;
}

bool wyNode::touchesPointerBegan(wyMotionEvent& e) {
	if(m_state.count == 1) {
		setSelected(true);

		if(m_multiTouchClickable) {
			if(m_downSelector != NULL) {
				m_downSelector->invoke();
			}
		}
	}

	return m_interceptTouch;
}

bool wyNode::touchesPointerEnded(wyMotionEvent& e) {
	if(m_state.count == 0) {
		setSelected(false);

		if(m_multiTouchClickable) {
			if (m_upSelector != NULL) {
				m_upSelector->invoke();
			}
		}
	}

	return m_interceptTouch;
}

bool wyNode::onDoubleTap(wyMotionEvent& e) {
	return false;
}

bool wyNode::onDoubleTapEvent(wyMotionEvent& e) {
	return false;
}

bool wyNode::onSingleTapConfirmed(wyMotionEvent& e) {
	return false;
}

bool wyNode::onDown(wyMotionEvent& e) {
	return false;
}

bool wyNode::onFling(wyMotionEvent& e1, wyMotionEvent& e2, float velocityX, float velocityY) {
	return false;
}

void wyNode::onLongPress(wyMotionEvent& e) {
}

bool wyNode::onScroll(wyMotionEvent& e1, wyMotionEvent& e2, float distanceX, float distanceY) {
	return false;
}

void wyNode::onShowPress(wyMotionEvent& e) {
}

bool wyNode::onSingleTapUp(wyMotionEvent& e) {
	return false;
}

wyNode::wyNode() :
		m_queueBucket(wyRenderQueue::INHERIT_BUCKET),
		m_transformMatrix(wyaZero),
		m_inverseMatrix(wyaZero),
		m_transformDirty(true),
		m_inverseDirty(true),
		m_enabled(true),
		m_selected(false),
		m_focused(false),
		m_visible(true),
		m_noDraw(false),
		m_relativeAnchorPoint(true),
		m_running(false),
		m_touchEnabled(false),
		m_keyEnabled(false),
		m_accelerometerEnabled(false),
		m_gestureEnabled(false),
		m_doubleTabEnabled(false),
		m_multiTouchClickable(false),
		m_interceptTouch(true),
		m_childrenChanging(false),
		m_touchPriority(0),
		m_keyPriority(0),
		m_gesturePriority(0),
		m_doubleTapPriority(0),
		m_accelerometerPriority(0),
		m_zOrder(0),
		m_tag(INVALID_TAG),
		m_anchorPointX(0.f),
		m_anchorPointY(0.f),
		m_anchorX(0.f),
		m_anchorY(0.f),
		m_positionX(0.f),
		m_positionY(0.f),
		m_velocityX(0.f),
		m_velocityY(0.f),
		m_accelerationX(0.f),
		m_accelerationY(0.f),
		m_width(0.f),
		m_height(0.f),
		m_rotation(0.f),
		m_scaleX(1.f),
		m_scaleY(1.f),
		m_skewX(0),
		m_skewY(0),
		m_vertexZ(0.f),
		m_parent(NULL),
		m_hasClip(false),
		m_clipRect(wyrZero),
		m_clipRelativeToSelf(false),
		m_children(wyArrayNew(3)),
		m_grid(NULL),
		m_camera(NULL),
		m_timers(NULL),
		m_renderPairs(WYNEW vector<RenderPair>()),
		m_color(wyc4bWhite),
		m_meshColorNeedUpdate(false),
		m_materialNeedUpdate(false),
		m_meshNeedUpdate(false),
		m_touchCoffin(NULL),
		m_downSelector(NULL),
		m_upSelector(NULL),
		m_moveOutSelector(NULL),
		m_positionCallback(NULL) {
	memset(&m_state, 0, sizeof(wyTouchState));
	setAnchor(0.5f, 0.5f);
}

wyNode* wyNode::make() {
	wyNode* n = WYNEW wyNode();
	return (wyNode*)n->autoRelease();
}

bool wyNode::hasPid(int pid) {
	for(int i = 0; i < m_state.count; i++) {
		if(m_state.pid[i] == pid)
			return true;
	}
	return false;
}

void wyNode::clearPid(int pid) {
	// find the pid
	int i;
	for(i = 0; i < m_state.count; i++) {
		if(m_state.pid[i] == pid)
			break;
	}

	// move pid forward
	if(i < m_state.count) {
		if(m_state.count > 0) {
			for(int j = i + 1; j < m_state.count; j++) {
				m_state.pid[j - 1] = m_state.pid[j];
			}
			m_state.count--;
		} else {
			m_state.count = 0;
		}
	}
}

void wyNode::setRelativeAnchorPoint(bool flag) {
	m_relativeAnchorPoint = flag;
	m_transformDirty = true;
	m_inverseDirty = true;
}

void wyNode::updateNodeToParentTransform() {
	if(m_transformDirty) {
		// Translate values
		float x = m_positionX;
		float y = m_positionY;
		if(!m_relativeAnchorPoint) {
			x += m_anchorPointX;
			y += m_anchorPointY;
		}

		// Rotation values
		float c = 1, s = 0;
		if(m_rotation != 0) {
			float radians = -wyMath::d2r(m_rotation);
			c = cosf(radians);
			s = sinf(radians);
		}

		// need skew or not?
		bool needsSkewMatrix = m_skewX != 0 || m_skewY != 0;

		// optimization:
		// inline anchor point calculation if skew is not needed
		if(!needsSkewMatrix && !(m_anchorPointX == 0 && m_anchorPointY == 0)) {
			x += c * -m_anchorPointX * m_scaleX + -s * -m_anchorPointY * m_scaleY;
			y += s * -m_anchorPointX * m_scaleX + c * -m_anchorPointY * m_scaleY;
		}

		// Build Transform Matrix
		m_transformMatrix = wya(c * m_scaleX, s * m_scaleX, -s * m_scaleY, c * m_scaleY, x, y);

		// XXX: Try to inline skew
		// If skew is needed, apply skew and then anchor point
		if(needsSkewMatrix) {
			wyAffineTransform skewMatrix = wya(1.0f, tanf(wyMath::d2r(-m_skewY)), tanf(wyMath::d2r(m_skewX)), 1.0f, 0.0f, 0.0f);
			wyaConcat(&skewMatrix, &m_transformMatrix);
			m_transformMatrix = skewMatrix;

			// adjust anchor point
			if(!(m_anchorPointX == 0 && m_anchorPointY == 0))
				wyaTranslate(&m_transformMatrix, -m_anchorPointX, -m_anchorPointY);
		}

		m_transformDirty = false;
	}
}

void wyNode::updateParentToNodeTransform() {
	if(m_inverseDirty) {
		updateNodeToParentTransform();
		m_inverseMatrix = m_transformMatrix;
		wyaInverse(&m_inverseMatrix);
		m_inverseDirty = false;
	}
}

wyAffineTransform wyNode::getNodeToWorldTransform() {
	updateNodeToParentTransform();
	wyAffineTransform t = m_transformMatrix;

	for(wyNode* p = m_parent; p != NULL; p = p->m_parent) {
		p->updateNodeToParentTransform();
		wyaConcat(&t, &(p->m_transformMatrix));
	}

	return t;
}

wyAffineTransform wyNode::getWorldToNodeTransform() {
	wyAffineTransform t = getNodeToWorldTransform();
	wyaInverse(&t);
	return t;
}

wyAffineTransform wyNode::getTransformMatrix() {
    updateNodeToParentTransform();
    return m_transformMatrix;
}

void wyNode::setTransformDirty() {
	m_transformDirty = true;
	m_inverseDirty = true;
}

float wyNode::getScale() {
	if(m_scaleX == m_scaleY) {
		return m_scaleX;
	} else {
		LOGW("ScaleX and ScaleY is not same, will return minimum scale");
		return MIN(m_scaleX, m_scaleY);
	}
}

void wyNode::translate(float x, float y) {
	setPosition(m_positionX + x, m_positionY + y);
}

void wyNode::translateChildren(float x, float y) {
	for(int i = 0; i < m_children->num; i++) {
		wyNode* child = (wyNode*)wyArrayGet(m_children, i);
		child->translate(x, y);
	}
}

void wyNode::setGrid(wyGridController* grid) {
	wyObjectRetain(grid);
	wyObjectRelease(m_grid);
	m_grid = grid;
}

void wyNode::addChildLocked(wyNode* child, int z, int tag) {
	pthread_mutex_lock(&gMutex);
	addChild(child, z, tag);
	pthread_mutex_unlock(&gMutex);
}

int wyNode::reorderChildLocked(wyNode* child, int z) {
	pthread_mutex_lock(&gMutex);
	int index = reorderChild(child, z);
	pthread_mutex_unlock(&gMutex);
	return index;
}

void wyNode::bringToFront(wyNode* child) {
	// find out max z order
	int maxZ = -INT_MAX;
	for(int i = 0; i < m_children->num; i++) {
		wyNode* child = (wyNode*)wyArrayGet(m_children, i);
		maxZ = MAX(child->getZOrder(), maxZ);
	}

	// set z order of this child to max z order
	reorderChild(child, maxZ);
}

void wyNode::bringToFrontLocked(wyNode* child) {
	pthread_mutex_lock(&gMutex);
	bringToFront(child);
	pthread_mutex_unlock(&gMutex);
}

void wyNode::bringToBack(wyNode* child) {
	// find out min z order
	int minZ = INT_MAX;
	for(int i = 0; i < m_children->num; i++) {
		wyNode* child = (wyNode*)wyArrayGet(m_children, i);
		minZ = MIN(child->getZOrder(), minZ);
	}

	// set z order of this child to min z order
	reorderChild(child, minZ);
}

void wyNode::bringToBackLocked(wyNode* child) {
	pthread_mutex_lock(&gMutex);
	bringToBack(child);
	pthread_mutex_unlock(&gMutex);
}

wyNode* wyNode::getChildByTagLocked(int tag) {
	pthread_mutex_lock(&gMutex);
	wyNode* child = getChildByTag(tag);
	pthread_mutex_unlock(&gMutex);

	return child;
}

wyNode** wyNode::getChildrenByTagLocked(int tag, size_t* count) {
	pthread_mutex_lock(&gMutex);
	wyNode** ret = getChildrenByTag(tag, count);
	pthread_mutex_unlock(&gMutex);

	return ret;
}

wyNode* wyNode::getFirstChild() {
	return m_children->num > 0 ? (wyNode*)wyArrayGet(m_children, 0) : NULL;
}

wyPoint wyNode::getAbsolutePosition() {
	wyPoint p = wyp(m_positionX, m_positionY);
	wyNode* n = this;
	while(n->getParent() != NULL) {
		n = n->getParent();
		p.x += n->m_positionX;
		p.y += n->m_positionY;
	}
	return p;
}

void wyNode::runAction(wyAction* action) {
	gActionManager->addActionLocked(action, this);
}

void wyNode::stopAction(int tag) {
	if(gActionManager)
		gActionManager->removeActionByTagLocked(this, tag);
}

void wyNode::stopAllActions(bool includeChildren) {
	if(gActionManager)
		gActionManager->removeActionsLocked(this, includeChildren);
}

void wyNode::pauseAllActions(bool includeChildren) {
	if(gActionManager)
		gActionManager->pauseActions(this, includeChildren);
}

void wyNode::resumeAllActions(bool includeChildren) {
	if(gActionManager)
		gActionManager->resumeActions(this, includeChildren);
}

void wyNode::pauseAction(int tag) {
	if(gActionManager)
		gActionManager->pauseActionLocked(this, tag);
}

void wyNode::resumeAction(int tag) {
	if(gActionManager)
		gActionManager->resumeActionLocked(this, tag);
}

wyAction* wyNode::getAction(int tag) {
	if(gActionManager)
		return gActionManager->getAction(this, tag);
	else
		return NULL;
}

bool wyNode::hasRunningAction() {
	if(gActionManager)
		return gActionManager->getRunningActionCount(this) > 0;
	else
		return false;
}

wyPoint wyNode::worldToNodeSpace(wyPoint p) {
	wyAffineTransform t = getWorldToNodeTransform();
	return wyaTransformPoint(t, p);
}

wyPoint wyNode::worldToNodeSpaceAR(wyPoint p) {
	p = worldToNodeSpace(p);
	return wypSub(p, wyp(m_anchorPointX, m_anchorPointY));
}

wyPoint wyNode::nodeToWorldSpace(wyPoint p) {
	wyAffineTransform t = getNodeToWorldTransform();
	return wyaTransformPoint(t, p);
}

wyPoint wyNode::nodeToWorldSpaceAR(wyPoint p) {
	p = wypAdd(p, wyp(m_anchorPointX, m_anchorPointY));
	p = nodeToWorldSpace(p);
	return p;
}

wyPoint wyNode::parentToNodeSpace(wyPoint p) {
	updateParentToNodeTransform();
	return wyaTransformPoint(m_inverseMatrix, p);
}

wyPoint wyNode::parentToNodeSpaceAR(wyPoint p) {
	p = parentToNodeSpace(p);
	return wypSub(p, wyp(m_anchorPointX, m_anchorPointY));
}

wyPoint wyNode::nodeToParentSpace(wyPoint p) {
	updateNodeToParentTransform();
	return wyaTransformPoint(m_transformMatrix, p);
}

wyPoint wyNode::nodeToParentSpaceAR(wyPoint p) {
	p = wypAdd(p, wyp(m_anchorPointX, m_anchorPointY));
	return nodeToParentSpace(p);
}

wyRect wyNode::getBoundingBox() {
	wyRect r = {
		0, 0, m_width, m_height
	};
	return r;
}

wyRect wyNode::getBoundingBoxRelativeToParent() {
	wyRect r = {
		0, 0, m_width, m_height
	};
	updateNodeToParentTransform();
	return wyaTransformRect(m_transformMatrix, r);
}

wyRect wyNode::getBoundingBoxRelativeToWorld() {
	wyRect r = {
		0, 0, m_width, m_height
	};
	wyAffineTransform t = getNodeToWorldTransform();
	return wyaTransformRect(t, r);
}

void wyNode::setAccelerometerEnabled(bool enabled) {
	if(m_accelerometerEnabled != enabled) {
		m_accelerometerEnabled = enabled;
		if(m_running) {
			if(enabled)
				gEventDispatcher->addAccelHandlerLocked(this, m_accelerometerPriority);
			else
				gEventDispatcher->removeAccelHandlerLocked(this);
		}
	}
}

void wyNode::setKeyEnabled(bool enabled) {
	if(m_keyEnabled != enabled) {
		m_keyEnabled = enabled;
		if(m_running) {
			if(enabled)
				gEventDispatcher->addKeyHandlerLocked(this, m_keyPriority);
			else
				gEventDispatcher->removeKeyHandlerLocked(this);
		}
	}
}

void wyNode::setTouchEnabled(bool enabled) {
	if(m_touchEnabled != enabled) {
		m_touchEnabled = enabled;
		if(m_running) {
			if(enabled)
				gEventDispatcher->addTouchHandlerLocked(this, m_touchPriority);
			else
				gEventDispatcher->removeTouchHandlerLocked(this);
		}
	}
}

void wyNode::setDoubleTapEnabled(bool enabled) {
	if(m_doubleTabEnabled != enabled) {
		m_doubleTabEnabled = enabled;
		if(m_running) {
			if(enabled)
				gEventDispatcher->addDoubleTapHandlerLocked(this, m_doubleTapPriority);
			else
				gEventDispatcher->removeDoubleTapHandlerLocked(this);
		}
	}
}

void wyNode::setGestureEnabled(bool enabled) {
	if(m_gestureEnabled != enabled) {
		m_gestureEnabled = enabled;
		if(m_running) {
			if(enabled)
				gEventDispatcher->addGestureHandlerLocked(this, m_gesturePriority);
			else
				gEventDispatcher->removeGestureHandlerLocked(this);
		}
	}
}

void wyNode::scheduleLocked(wyTimer* t) {
	if(t == NULL) {
		LOGW("node schedule: timer must be non-null");
		return;
	}

	// check array
	if(m_timers == NULL)
		m_timers = wyArrayNew(3);

	// if already contains, return
	if(wyArrayIndexOf(m_timers, t, wyTimerEquals, NULL) >= 0) {
		if(t->isOneShot() && t->isDone()) {
			t->reset();

			// if node is running, schedule it now because onEnter won't be called later
			if(m_running) {
				gScheduler->scheduleLocked(t);
			}
		} else {
			LOGW("this timer is already scheduled");
		}

		return;
	}

	// if node is running, schedule it now because onEnter won't be called later
	if(m_running) {
		gScheduler->scheduleLocked(t);
	}

	pthread_mutex_lock(&gMutex);
	wyArrayPush(m_timers, t);
	wyObjectRetain(t);
	pthread_mutex_unlock(&gMutex);
}

void wyNode::resumeAllTimers(bool includeChildren) {
	// set
	if(m_timers) {
		pthread_mutex_lock(&gMutex);
		for(int i = 0; i < m_timers->num; i++) {
			wyTimer* t = (wyTimer*)wyArrayGet(m_timers, i);
			t->setPaused(false);
		}
		pthread_mutex_unlock(&gMutex);
	}

	if(includeChildren) {
		for(int i = 0; i < m_children->num; i++) {
			wyNode* child = (wyNode*)wyArrayGet(m_children, i);
			child->resumeAllTimers(includeChildren);
		}
	}
}

void wyNode::pauseAllTimers(bool includeChildren) {
	// set
	if(m_timers) {
		pthread_mutex_lock(&gMutex);
		for(int i = 0; i < m_timers->num; i++) {
			wyTimer* t = (wyTimer*)wyArrayGet(m_timers, i);
			t->setPaused(true);
		}
		pthread_mutex_unlock(&gMutex);
	}

	if(includeChildren) {
		for(int i = 0; i < m_children->num; i++) {
			wyNode* child = (wyNode*)wyArrayGet(m_children, i);
			child->pauseAllTimers(includeChildren);
		}
	}
}

void wyNode::unscheduleLocked(wyTimer* t) {
	// explicit null handling
	if(t == NULL) {
		LOGW("node unschedule: timer must be non-null");
		return;
	}

	// no timer scheduled?
	if(m_timers == NULL)
		return;

	// doesn't contain this timer?
	if(wyArrayIndexOf(m_timers, t, wyTimerEquals, NULL) == -1)
		return;

	pthread_mutex_lock(&gMutex);

	// delete t from array
	// there is a little hack to ensure the deleted timer is released
	wyTimer* deletedTimer = (wyTimer*)wyArrayDeleteObj(m_timers, t, wyTimerEquals, NULL);

	// unschedule this timer
	if(m_running)
		gScheduler->unscheduleLocked(t);

	// release the timer because we retained it before
	wyObjectRelease(deletedTimer);

	pthread_mutex_unlock(&gMutex);
}

void wyNode::setDownSelector(wyTargetSelector* ts) {
	wyObjectRetain(ts);
	wyObjectRelease(m_downSelector);
	m_downSelector = ts;
}

void wyNode::setUpSelector(wyTargetSelector* ts) {
	wyObjectRetain(ts);
	wyObjectRelease(m_upSelector);
	m_upSelector = ts;
}

void wyNode::setMoveOutSelector(wyTargetSelector* ts) {
	wyObjectRetain(ts);
	wyObjectRelease(m_moveOutSelector);
	m_moveOutSelector = ts;
}

wyCamera* wyNode::getCamera() {
	if(m_camera == NULL) {
		m_camera = wyCamera::make();
		m_camera->retain();
	}
	return m_camera;
}

void wyNode::javaRelease(bool includeChildren) {
	// release
	wyObject::javaRelease();

	// check children
	if(includeChildren) {
		for(int i = 0; i < m_children->num; i++) {
			wyNode* child = (wyNode*)wyArrayGet(m_children, i);
			child->javaRelease(true);
		}
	}
}

float wyNode::getOriginX() {
	if(m_relativeAnchorPoint)
		return m_positionX - m_anchorPointX;
	else
		return m_positionX;
}

float wyNode::getOriginY() {
	if(m_relativeAnchorPoint)
		return m_positionY - m_anchorPointY;
	else
		return m_positionY;
}

void wyNode::sizeToFit() {
	wySize size = wysZero;
	for(int i = 0; i < m_children->num; i++) {
		wyNode* child = (wyNode*)wyArrayGet(m_children, i);
		size.width = MAX(size.width, child->getOriginX() + child->getWidth());
		size.height = MAX(size.height, child->getOriginY() + child->getHeight());
	}
	setContentSize(size.width, size.height);
}

void wyNode::enlargeNode(float left, float right, float top, float bottom) {
	// need offset child
	for(int i = 0; i < m_children->num; i++) {
		wyNode* child = (wyNode*)wyArrayGet(m_children, i);
		child->setPosition(child->getPositionX() + left, child->getPositionY() + bottom);
	}

	// set new content size
	setContentSize(m_width + left + right, m_height + top + bottom);
}

wySize wyNode::getFitSize() {
	float left = MAX_FLOAT;
	float right = -MAX_FLOAT;
	float top = -MAX_FLOAT;
	float bottom = MAX_FLOAT;
	for(int i = 0; i < m_children->num; i++) {
		wyNode* child = (wyNode*)wyArrayGet(m_children, i);
		left = MIN(left, child->getOriginX());
		bottom = MIN(bottom, child->getOriginY());
		right = MAX(right, child->getOriginX() + child->getWidth());
		top = MAX(top, child->getOriginY() + child->getHeight());
	}

	return wys(right - left, top - bottom);
}

void wyNode::setFocused(bool focused) {
	if(wyDirector::getInstance()->isFocusEnabled())
		m_focused = focused;
}

bool wyNode::isAncestor(wyNode* node) {
	while(node->getParent() != this && node->getParent() != NULL) {
		node = node->getParent();
	}
	return node->getParent() == this;
}

void wyNode::setTexture(wyTexture2D* tex, int index) {
	// get material at given index
	wyMaterial* m = getMaterial(index);
	if(!m)
		return;

	// get texture parameter, if none, create
	wyMaterialParameter* mp = m->getParameter(wyUniform::NAME[wyUniform::TEXTURE_2D]);
	if(!mp) {
		wyMaterialTextureParameter* p = wyMaterialTextureParameter::make(wyUniform::NAME[wyUniform::TEXTURE_2D], tex);
		m->addParameter(p);
	} else {
		wyMaterialTextureParameter* mtp = (wyMaterialTextureParameter*)mp;
		mtp->setTexture(tex);
	}

	// flag update
	setNeedUpdateMesh(true);
}

wyTexture2D* wyNode::getTexture(int index) {
	// get material at given index
	wyMaterial* m = getMaterial(index);
	if(!m)
		return NULL;

	// get texture parameter
	wyMaterialParameter* mp = m->getParameter(wyUniform::NAME[wyUniform::TEXTURE_2D]);
	if(mp) {
		wyMaterialTextureParameter* mtp = (wyMaterialTextureParameter*)mp;
		return mtp->getTexture();
	} else {
		return NULL;
	}
}

wyMesh* wyNode::getMesh(int index) {
	// check index
	if(index < 0 || index >= m_renderPairs->size()) {
		LOGW("wyNode::getMesh, index is out of range");
		return NULL;
	}

	RenderPair& p = m_renderPairs->at(index);
	return p.mesh;
}

wyMaterial* wyNode::getMaterial(int index) {
	// check index
	if(index < 0 || index >= m_renderPairs->size()) {
		LOGW("wyNode::getMaterial, index is out of range");
		return NULL;
	}

	RenderPair& p = m_renderPairs->at(index);
	return p.material;
}

int wyNode::getLodLevel(int index) {
	// check index
	if(index < 0 || index >= m_renderPairs->size()) {
		LOGW("wyNode::getLodLevel, index is out of range: %d", index);
		return 0;
	}

	RenderPair& p = m_renderPairs->at(index);
	return p.lod;
}

void wyNode::setLodLevel(int level, int index) {
	// check index
	if(index < 0 || index >= m_renderPairs->size()) {
		LOGW("wyNode::setLodLevel: index is out of range: %d", index);
		return;
	}

	// check mesh levels
	RenderPair& p = m_renderPairs->at(index);
	wyMesh* mesh = p.mesh;
	if(mesh->getNumberOfLodLevel() == 0) {
		LOGW("wyNode::setLodLevel: no LOD data set on geometry mesh");
		return;
	}

	// validate destination level
	if(level < 0 || level >= mesh->getNumberOfLodLevel()) {
		LOGW("wyNode::setLodLevel: level %d is not valid");
		return;
	}

	// set it
	p.lod = level;
}

void wyNode::addRenderPair(wyMaterial* material, wyMesh* mesh) {
	RenderPair p = {
			material,
			mesh,
			0
	};
	m_renderPairs->push_back(p);
	wyObjectRetain(material);
	wyObjectRetain(mesh);
}

void wyNode::removeRenderPairs(wyRenderPairSelector* sel, void* data) {
	for(vector<RenderPair>::iterator iter = m_renderPairs->begin(); iter != m_renderPairs->end();) {
		if(sel->selectRenderPair(iter->material, iter->mesh, data)) {
			iter->material->release();
			iter->mesh->release();
			iter = m_renderPairs->erase(iter);
		} else {
			iter++;
		}
	}
}

void wyNode::removeRenderPairsByTag(int tag) {
	for(vector<RenderPair>::iterator iter = m_renderPairs->begin(); iter != m_renderPairs->end();) {
		if(iter->mesh->getTag() == tag) {
			iter->material->release();
			iter->mesh->release();
			iter = m_renderPairs->erase(iter);
		} else {
			iter++;
		}
	}
}

wyMesh* wyNode::getMeshByTag(int tag) {
	for(vector<RenderPair>::iterator iter = m_renderPairs->begin(); iter != m_renderPairs->end(); iter++) {
		if(iter->mesh->getTag() == tag)
			return iter->mesh;
	}
	return NULL;
}

void wyNode::clearRenderPairs() {
	for(vector<RenderPair>::iterator iter = m_renderPairs->begin(); iter != m_renderPairs->end(); iter++) {
		wyObjectRelease(iter->material);
		wyObjectRelease(iter->mesh);
	}
	m_renderPairs->clear();
}

void wyNode::replaceMaterial(wyMaterial* material, int index) {
	// validate
	if(index < 0 || index >= m_renderPairs->size())
		return;

	// replace
	RenderPair& rp = m_renderPairs->at(index);
	wyObjectRetain(material);
	wyObjectRelease(rp.material);
	rp.material = material;
}

void wyNode::replaceMesh(wyMesh* mesh, int index) {
	// validate
	if(index < 0 || index >= m_renderPairs->size())
		return;

	// replace
	RenderPair& rp = m_renderPairs->at(index);
	wyObjectRetain(mesh);
	wyObjectRelease(rp.mesh);
	rp.mesh = mesh;
}

wyRenderQueue::Bucket wyNode::getQueueBucket() {
    if (m_queueBucket != wyRenderQueue::INHERIT_BUCKET) {
        return m_queueBucket;
    } else if (m_parent) {
        return m_parent->getQueueBucket();
    } else {
        return wyRenderQueue::OPAQUE_BUCKET;
    }
}

void wyNode::setAlpha(int alpha) {
	m_color.a = alpha;
	setNeedUpdateMeshColor(true);
}

void wyNode::setColor(wyColor3B color) {
	m_color.r = color.r;
	m_color.g = color.g;
	m_color.b = color.b;
	setNeedUpdateMeshColor(true);
}

void wyNode::setColor(wyColor4B color) {
	m_color.r = color.r;
	m_color.g = color.g;
	m_color.b = color.b;
	m_color.a = color.a;
	setNeedUpdateMeshColor(true);
}

bool wyNode::isDither() {
	if(getRenderPairCount() > 0) {
		wyTechnique* tech = getMaterial(0)->getTechnique();
		wyRenderState* rs = tech->getRenderState();
		return rs->ditherEnabled;
	} else {
		return false;
	}
}

void wyNode::setDither(bool flag) {
	int count = getRenderPairCount();
    for(int i = 0; i < count; i++) {
		wyTechnique* tech = getMaterial(i)->getTechnique();
		wyRenderState* rs = tech->getRenderState();
		rs->ditherEnabled = flag;
    }
}

wyRenderState::BlendMode wyNode::getBlendMode() {
	if(getRenderPairCount() > 0) {
		wyTechnique* tech = getMaterial(0)->getTechnique();
		wyRenderState* rs = tech->getRenderState();
		return rs->blendMode;
	} else {
		return wyRenderState::NO_BLEND;
	}
}

void wyNode::setBlendMode(wyRenderState::BlendMode mode) {
	int count = getRenderPairCount();
    for(int i = 0; i < count; i++) {
		wyTechnique* tech = getMaterial(i)->getTechnique();
		wyRenderState* rs = tech->getRenderState();
		rs->blendMode = mode;
    }
}

wyParallaxObject* wyNode::createParallaxObject() { 
	return wyParallaxObject::make(); 
}

bool wyNode::isGridActive() { 
	return m_grid != NULL && m_grid->isActive(); 
}

void wyNode::applyWorldMatrix() {
	// switch to world stack
	kmGLMatrixMode(KM_GL_WORLD);

	// multiply current node world matrix
	wyAffineTransform t = getTransformMatrix();
	kmMat4 m;
	wyaToGL(t, m.mat);
	m.mat[14] = m_vertexZ;
	kmGLMultMatrix(&m);

	// if node has camera, apply it
	if(hasCamera()) {
		bool translate = m_anchorPointX != 0 || m_anchorPointY != 0;
		if(translate)
			kmGLTranslatef(m_anchorPointX, m_anchorPointY, 0);

		kmGLMultMatrix(getCamera()->getViewMatrix());

		if(translate)
			kmGLTranslatef(-m_anchorPointX, -m_anchorPointY, 0);
	}
}
