/* Copyright (c) 2008-2016, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package java.util;

import java.io.BufferedReader;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.IOException;
import java.lang.reflect.InvocationTargetException;
import java.net.URL;

public final class ServiceLoader<S> implements Iterable<S> {
  private final Class<S> service;
  private final ClassLoader loader;

  private ServiceLoader(Class<S> service, ClassLoader loader) {
    if (service == null) {
      throw new NullPointerException();
    }
    this.service = service;
    this.loader = loader;
  }

  public static <S> ServiceLoader<S> load(Class<S> service,
                                           ClassLoader loader) {
    return new ServiceLoader<S>(service, loader);
  }

  public static <S> ServiceLoader<S> load(Class<S> service) {
    ClassLoader loader = Thread.currentThread().getContextClassLoader();
    if (loader == null) {
      loader = ClassLoader.getSystemClassLoader();
    }
    return load(service, loader);
  }

  public static <S> ServiceLoader<S> loadInstalled(Class<S> service) {
    return load(service, ClassLoader.getSystemClassLoader());
  }

  public void reload() { }

  public Iterator<S> iterator() {
    return new ServiceIterator();
  }

  private final class ServiceIterator implements Iterator<S> {
    private final ArrayList<S> providers = new ArrayList<S>();
    private int index;

    ServiceIterator() {
      String path = "META-INF/services/" + service.getName();
      try {
        Enumeration<URL> urls = loader == null
          ? ClassLoader.getSystemResources(path)
          : loader.getResources(path);
        while (urls != null && urls.hasMoreElements()) {
          URL url = urls.nextElement();
          if (url == null) {
            continue;
          }
          read(url);
        }
      } catch (IOException e) {
        throw new ServiceConfigurationError(service.getName(), e);
      }
    }

    private void read(URL url) throws IOException {
      InputStream in = url.openStream();
      if (in == null) {
        return;
      }
      try {
        BufferedReader reader = new BufferedReader(new InputStreamReader(in));
        String line;
        while ((line = reader.readLine()) != null) {
          int hash = line.indexOf('#');
          if (hash >= 0) {
            line = line.substring(0, hash);
          }
          line = line.trim();
          if (line.length() == 0) {
            continue;
          }
          providers.add(instantiate(line));
        }
      } finally {
        in.close();
      }
    }

    private S instantiate(String name) {
      try {
        ClassLoader cl = loader == null
          ? ClassLoader.getSystemClassLoader() : loader;
        Class<?> type = Class.forName(name, true, cl);
        Object instance = type.getDeclaredConstructor().newInstance();
        return service.cast(instance);
      } catch (ClassNotFoundException e) {
        throw new ServiceConfigurationError(name, e);
      } catch (NoSuchMethodException e) {
        throw new ServiceConfigurationError(name, e);
      } catch (InstantiationException e) {
        throw new ServiceConfigurationError(name, e);
      } catch (IllegalAccessException e) {
        throw new ServiceConfigurationError(name, e);
      } catch (InvocationTargetException e) {
        throw new ServiceConfigurationError(name, e.getCause());
      }
    }

    public boolean hasNext() {
      return index < providers.size();
    }

    public S next() {
      if (!hasNext()) {
        throw new NoSuchElementException();
      }
      return providers.get(index++);
    }

    public void remove() {
      throw new UnsupportedOperationException();
    }
  }
}
