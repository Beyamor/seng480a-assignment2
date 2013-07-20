public class Arrays {

	public static void main(String args[]) {

		String strings[] = {"these ", "are ", "strings"};

		for (int i = 0; i < 100; ++i) {

			StringBuilder _ = new StringBuilder();
		}

		StringBuilder sb = new StringBuilder();
		for (int i = 0; i < strings.length; ++i) {

			sb.append(strings[i]);
		}

		System.out.println(sb.toString());
	}
}
