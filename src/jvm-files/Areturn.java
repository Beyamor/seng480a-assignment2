public class Areturn {

	public String name;

	public Areturn(String name) {

		this.name = name;
	}

	public static Areturn makeFoo() {

		return new Areturn("foo");
	}

	public static Areturn makeBar() {

		return new Areturn("bar");
	}	

	public static void main(String[] args) {

		Areturn foo = makeFoo();
		Areturn bar = makeBar();

		for (int i = 0; i < 100; ++i) {

			Areturn whatever = new Areturn("whatever");
		}

		System.out.println(foo.name + bar.name);
	}
}
