/* Copyright (c) 2008-2016, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package java.util.concurrent.locks;

import java.util.concurrent.TimeUnit;

public class ReentrantLock implements Lock {
  private int holds;
  private Thread owner;

  public ReentrantLock() {}

  public ReentrantLock(boolean fair) {}

  public void lock() {
    synchronized (this) {
      Thread current = Thread.currentThread();
      while (owner != null && owner != current) {
        try {
          wait();
        } catch (InterruptedException e) {
          current.interrupt();
        }
      }
      owner = current;
      holds++;
    }
  }

  public void lockInterruptibly() throws InterruptedException {
    if (Thread.interrupted()) throw new InterruptedException();
    lock();
  }

  public boolean tryLock() {
    synchronized (this) {
      Thread current = Thread.currentThread();
      if (owner != null && owner != current) return false;
      owner = current;
      holds++;
      return true;
    }
  }

  public boolean tryLock(long time, TimeUnit unit) throws InterruptedException {
    if (Thread.interrupted()) throw new InterruptedException();
    return tryLock();
  }

  public void unlock() {
    synchronized (this) {
      if (owner != Thread.currentThread() || holds <= 0) {
        throw new IllegalMonitorStateException();
      }
      holds--;
      if (holds == 0) {
        owner = null;
        notifyAll();
      }
    }
  }

  public Condition newCondition() {
    throw new UnsupportedOperationException();
  }

  public boolean isHeldByCurrentThread() {
    synchronized (this) {
      return owner == Thread.currentThread();
    }
  }

  public int getHoldCount() {
    synchronized (this) {
      return owner == Thread.currentThread() ? holds : 0;
    }
  }
}
