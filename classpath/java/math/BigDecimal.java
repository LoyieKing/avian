/* Copyright (c) 2008-2016, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

package java.math;

public class BigDecimal extends Number {
  private final int sign;
  private final String digits;
  private final int scale;

  public BigDecimal(String val) {
    if (val == null) {
      throw new NullPointerException();
    }
    int i = 0;
    int n = val.length();
    int parsedSign = 1;
    if (i < n && (val.charAt(i) == '+' || val.charAt(i) == '-')) {
      if (val.charAt(i) == '-') {
        parsedSign = -1;
      }
      i++;
    }
    int mantissa = i;
    int dot = -1;
    while (i < n) {
      char c = val.charAt(i);
      if (c == '.') {
        if (dot >= 0) {
          throw new NumberFormatException(val);
        }
        dot = i;
      } else if (c < '0' || c > '9') {
        break;
      }
      i++;
    }
    int mantissaEnd = i;
    if (mantissaEnd == mantissa
        || (dot >= 0 && mantissaEnd == mantissa + 1)) {
      throw new NumberFormatException(val);
    }
    int exp = 0;
    if (i < n && (val.charAt(i) == 'e' || val.charAt(i) == 'E')) {
      i++;
      int expSign = 1;
      if (i < n && (val.charAt(i) == '+' || val.charAt(i) == '-')) {
        if (val.charAt(i) == '-') {
          expSign = -1;
        }
        i++;
      }
      if (i >= n || val.charAt(i) < '0' || val.charAt(i) > '9') {
        throw new NumberFormatException(val);
      }
      while (i < n && val.charAt(i) >= '0' && val.charAt(i) <= '9') {
        exp = exp * 10 + (val.charAt(i) - '0');
        i++;
      }
      exp *= expSign;
    }
    if (i != n) {
      throw new NumberFormatException(val);
    }

    StringBuilder raw = new StringBuilder();
    for (int j = mantissa; j < mantissaEnd; j++) {
      if (j != dot) {
        raw.append(val.charAt(j));
      }
    }
    int parsedScale = (dot < 0 ? 0 : mantissaEnd - dot - 1) - exp;
    int zero = 0;
    while (zero < raw.length() - 1 && raw.charAt(zero) == '0') {
      zero++;
    }
    String parsedDigits = raw.substring(zero);
    if (parsedDigits.equals("0")) {
      parsedSign = 0;
      parsedScale = 0;
    }
    this.sign = parsedSign;
    this.digits = parsedDigits;
    this.scale = parsedScale;
  }

  private BigDecimal(int sign, String digits, int scale) {
    this.sign = sign;
    this.digits = digits;
    this.scale = scale;
  }

  public BigDecimal add(BigDecimal augend) {
    if (augend == null) {
      throw new NullPointerException();
    }
    if (sign == 0) {
      return augend;
    }
    if (augend.sign == 0) {
      return this;
    }
    int resultScale = scale > augend.scale ? scale : augend.scale;
    String left = shift(digits, resultScale - scale);
    String right = shift(augend.digits, resultScale - augend.scale);
    if (sign == augend.sign) {
      return normalize(sign, addDigits(left, right), resultScale);
    }
    int cmp = compareDigits(left, right);
    if (cmp == 0) {
      return new BigDecimal(0, "0", 0);
    }
    if (cmp > 0) {
      return normalize(sign, subDigits(left, right), resultScale);
    }
    return normalize(augend.sign, subDigits(right, left), resultScale);
  }

  public String toPlainString() {
    if (sign == 0) {
      return "0";
    }
    String body;
    if (scale <= 0) {
      body = shift(digits, -scale);
    } else if (scale >= digits.length()) {
      body = "0." + zeros(scale - digits.length()) + digits;
    } else {
      int cut = digits.length() - scale;
      body = digits.substring(0, cut) + "." + digits.substring(cut);
    }
    if (sign < 0) {
      return "-" + body;
    }
    return body;
  }

  public String toString() {
    return toPlainString();
  }

  public byte byteValue() {
    return (byte) intValue();
  }

  public short shortValue() {
    return (short) intValue();
  }

  public int intValue() {
    return (int) longValue();
  }

  public long longValue() {
    if (sign == 0) {
      return 0;
    }
    String whole = scale <= 0 ? shift(digits, -scale)
        : (scale >= digits.length() ? "" : digits.substring(0, digits.length() - scale));
    long value = 0;
    for (int i = 0; i < whole.length(); i++) {
      value = value * 10 + (whole.charAt(i) - '0');
    }
    if (sign < 0) {
      return -value;
    }
    return value;
  }

  public float floatValue() {
    return Float.parseFloat(toPlainString());
  }

  public double doubleValue() {
    return Double.parseDouble(toPlainString());
  }

  private static BigDecimal normalize(int sign, String digits, int scale) {
    int zero = 0;
    while (zero < digits.length() - 1 && digits.charAt(zero) == '0') {
      zero++;
    }
    String trimmed = digits.substring(zero);
    if (trimmed.equals("0")) {
      return new BigDecimal(0, "0", 0);
    }
    return new BigDecimal(sign, trimmed, scale);
  }

  private static String shift(String digits, int zeros) {
    if (zeros == 0) {
      return digits;
    }
    return digits + zeros(zeros);
  }

  private static String zeros(int count) {
    StringBuilder sb = new StringBuilder();
    for (int i = 0; i < count; i++) {
      sb.append('0');
    }
    return sb.toString();
  }

  private static int compareDigits(String left, String right) {
    if (left.length() != right.length()) {
      return left.length() > right.length() ? 1 : -1;
    }
    return left.compareTo(right);
  }

  private static String addDigits(String left, String right) {
    int n = left.length() > right.length() ? left.length() : right.length();
    char[] out = new char[n + 1];
    int carry = 0;
    for (int i = 0; i < n; i++) {
      int ld = i < left.length() ? left.charAt(left.length() - 1 - i) - '0' : 0;
      int rd = i < right.length() ? right.charAt(right.length() - 1 - i) - '0' : 0;
      int sum = ld + rd + carry;
      out[n - i] = (char) ('0' + (sum % 10));
      carry = sum / 10;
    }
    out[0] = (char) ('0' + carry);
    return new String(out);
  }

  private static String subDigits(String left, String right) {
    char[] out = new char[left.length()];
    int borrow = 0;
    for (int i = 0; i < left.length(); i++) {
      int ld = left.charAt(left.length() - 1 - i) - '0';
      int rd = i < right.length() ? right.charAt(right.length() - 1 - i) - '0' : 0;
      int diff = ld - rd - borrow;
      if (diff < 0) {
        diff += 10;
        borrow = 1;
      } else {
        borrow = 0;
      }
      out[left.length() - 1 - i] = (char) ('0' + diff);
    }
    return new String(out);
  }
}
