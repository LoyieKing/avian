import java.math.BigDecimal;

public class BigDecimalTest {
  private static void expect(boolean v) {
    if (!v) {
      throw new RuntimeException();
    }
  }

  public static void main(String[] args) {
    BigDecimal sum = new BigDecimal("0");
    sum = sum.add(new BigDecimal("0.1"));
    sum = sum.add(new BigDecimal("0.2"));
    sum = sum.add(new BigDecimal("0.7"));
    expect(sum.toPlainString().equals("1.0"));
    expect(sum.floatValue() == 1.0f);
    expect(!(sum.add(new BigDecimal("0.0001")).floatValue() <= 1.0f));

    BigDecimal floated = new BigDecimal("0");
    float[] parts = new float[] { 0.25f, 0.25f, 0.5f };
    for (int i = 0; i < parts.length; i++) {
      floated = floated.add(new BigDecimal(String.valueOf(parts[i])));
    }
    expect(floated.floatValue() <= 1.0f);

    expect(new BigDecimal("1.5E1").doubleValue() == 15.0);
    expect(new BigDecimal("1E-1").toPlainString().equals("0.1"));
    expect(new BigDecimal("-1.50").add(new BigDecimal("0.25")).toPlainString()
        .equals("-1.25"));
    expect(new BigDecimal("10").intValue() == 10);
    expect(new BigDecimal(String.valueOf(0.25f)).doubleValue()
        == Double.parseDouble(String.valueOf(0.25f)));
  }
}
